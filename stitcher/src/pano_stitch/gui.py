"""Small desktop frontend for the metadata-driven panorama stitcher."""

from __future__ import annotations

import json
import logging
import os
import queue
import threading
import tkinter as tk
from dataclasses import replace
from enum import Enum, auto
from fractions import Fraction
from functools import partial
from pathlib import Path
from tkinter import filedialog, font, messagebox, ttk
from typing import Any, cast

import numpy as np
from PIL import Image, ImageTk

from pano_stitch.application import (
    ApplicationSettings,
    ExposureEdits,
    begin_operation,
    cancellation_requested,
    default_output_name,
    finish_operation,
    plan_outputs,
    requested_render_operation,
    validate_session_request,
)
from pano_stitch.compositor import (
    AutomaticExposureAmbiguousError,
    ExposureReport,
    GpuSessionCache,
    RenderCancelledError,
    estimate_automatic_exposure_gains,
    estimate_render_resources,
    estimate_target_exposure_gain,
    frame_coverage_masks,
    render_preview,
    render_session,
    thumbnail_output_path,
)
from pano_stitch.metadata import SessionMetadata, load_session
from pano_stitch.sessions import (
    SessionRecord,
    delete_files,
    deletion_targets,
    discover_sessions,
    mark_stitched,
)

LOGGER = logging.getLogger(__name__)
MIB = 1024**2
GIB = 1024**3
GPU_BUILD = True
GPU_PREVIEW_WIDTH_MULTIPLIER = 4


class UiState(Enum):
    EMPTY = auto()
    READY = auto()
    PREVIEW = auto()
    BUSY = auto()
    CLOSING = auto()


def _magnified_crop_box(
    source_size: tuple[int, int],
    viewport_size: tuple[int, int],
    pointer: tuple[float, float],
) -> tuple[int, int, int, int]:
    """Center a viewport-sized crop on a proportional preview pointer position."""

    source_width, source_height = source_size
    viewport_width, viewport_height = viewport_size
    center_x = round(min(1.0, max(0.0, pointer[0])) * source_width)
    center_y = round(min(1.0, max(0.0, pointer[1])) * source_height)
    left = min(max(0, center_x - viewport_width // 2), source_width - viewport_width)
    top = min(max(0, center_y - viewport_height // 2), source_height - viewport_height)
    return left, top, left + viewport_width, top + viewport_height


def _mask_boundary(mask: np.ndarray) -> np.ndarray:
    boundary = mask & ~np.roll(mask, 1, axis=1)
    boundary |= mask & ~np.roll(mask, -1, axis=1)
    boundary |= mask & ~np.vstack((mask[:1], mask[:-1]))
    boundary |= mask & ~np.vstack((mask[1:], mask[-1:]))
    return cast(np.ndarray, boundary)


def _compose_preview_display(
    source: Image.Image,
    viewport: tuple[int, int],
    masks: tuple[np.ndarray, ...],
    show_boundaries: bool,
    crop_box: tuple[int, int, int, int] | None,
    hovered_poses: frozenset[int],
    target_pose: int | None,
    target_mode: bool,
) -> Image.Image:
    display = (
        source.resize(viewport, Image.Resampling.LANCZOS)
        if crop_box is None
        else source.crop(crop_box)
    )
    if not masks:
        return display
    rgba = np.asarray(display.convert("RGBA"), dtype=np.uint8).copy()
    for position, source_mask in enumerate(masks):
        mask_image = Image.fromarray(source_mask.astype(np.uint8) * 255, mode="L")
        if crop_box is not None:
            left, top, right, bottom = crop_box
            mask_box = (
                left * source_mask.shape[1] / source.width,
                top * source_mask.shape[0] / source.height,
                right * source_mask.shape[1] / source.width,
                bottom * source_mask.shape[0] / source.height,
            )
            mask_image = mask_image.transform(
                display.size,
                Image.Transform.EXTENT,
                mask_box,
                Image.Resampling.NEAREST,
            )
        elif mask_image.size != display.size:
            mask_image = mask_image.resize(display.size, Image.Resampling.NEAREST)
        mask = np.asarray(mask_image, dtype=np.uint8) > 0
        if position in hovered_poses:
            color = np.array((0, 102, 255) if target_mode else (255, 0, 255), dtype=np.float32)
            rgba[mask, :3] = np.asarray(rgba[mask, :3] * 0.8 + color * 0.2, dtype=np.uint8)
        if show_boundaries or position in hovered_poses:
            boundary_color = (0, 102, 255, 255) if position == target_pose else (255, 0, 255, 255)
            rgba[_mask_boundary(mask)] = boundary_color
    return Image.fromarray(rgba, mode="RGBA").convert("RGB")


def _backend_status(backend: str, detail: str, *, gpu_requested: bool, log_directory: Path) -> str:
    if backend == "cpu":
        if gpu_requested:
            return f"CPU (failed to initialize GPU; see error log in {log_directory})"
        return "CPU"
    if backend.startswith("gpu"):
        _, separator, remainder = detail.partition("reserve=")
        byte_count, unit_separator, _ = remainder.partition(" bytes")
        if separator and unit_separator:
            try:
                reserve_bytes = int(byte_count)
            except ValueError:
                pass
            else:
                reserve = (
                    f"{reserve_bytes / GIB:.2f} GB"
                    if reserve_bytes >= GIB
                    else f"{reserve_bytes / MIB:.0f} MB"
                )
                return f"D3D12 (reserved {reserve} VRAM)"
    return f"Backend: {backend.upper()} — {detail}"


class StitcherApp:
    """Tkinter UI that delegates all image work to the existing stitcher API."""

    def __init__(self, root: tk.Tk, *, gpu_build: bool = GPU_BUILD) -> None:
        self.root = root
        self.gpu_build = gpu_build
        self.root.title("Cyberpunk Panorama Stitcher")
        self.root.minsize(680, 400)
        self._events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self._cancel_event: threading.Event | None = None
        self._worker: threading.Thread | None = None
        self._validation_after: str | None = None
        self._close_when_idle = False
        self._close_finished = False
        self._state = UiState.EMPTY
        self._state_before_busy = UiState.EMPTY
        self._active_operation: str | None = None
        self._validated = False
        self._history: dict[str, Any] = {}
        self._sessions: tuple[SessionRecord, ...] = ()
        self._preview_report: ExposureReport | None = None
        self._preview_photo: Any | None = None
        self._preview_magnified_photo: Any | None = None
        self._preview_image: Image.Image | None = None
        self._preview_overview: Image.Image | None = None
        self._gpu_preview_display: Any = None
        self._preview_viewport = (1, 1)
        self._preview_width = 1
        self._preview_session: SessionMetadata | None = None
        self._coverage_masks: tuple[Any, ...] = ()
        self._active_preview_crop: tuple[int, int, int, int] | None = None
        self._display_lock = threading.Lock()
        self._display_generation = 0
        self._display_valid_generation = 0
        self._display_applied_generation = 0
        self._display_pending: tuple[Any, ...] | None = None
        self._display_worker: threading.Thread | None = None
        self._manual_gains: tuple[float, ...] = ()
        self._selected_poses: set[int] = set()
        self._target_pose: int | None = None
        self._hovered_poses: set[int] = set()
        self._target_mode = False
        self._pose_widgets: list[tk.Label] = []
        self._native_output_size: tuple[int, int] | None = None
        self._resolution_geometry: tuple[str, float] | None = None
        self._gpu_session_cache = GpuSessionCache()
        self._build_variables()
        self._load_settings()
        self._build_widgets()
        self._form_controls = self._form_control_widgets()
        self._output_dir_writable = self._is_output_directory_writable()
        self.session_var.trace_add("write", self._input_changed)
        self.game_dir_var.trace_add("write", lambda *_args: self._refresh_sessions())
        self.image_dir_var.trace_add("write", self._input_changed)
        self.output_dir_var.trace_add("write", self._output_directory_changed)
        self.output_name_var.trace_add("write", self._output_name_changed)
        self.width_var.trace_add("write", lambda *_args: self._update_expected_resolution())
        self.format_var.trace_add("write", self._format_changed)
        preview_variables = [
            self.blend_var,
            self.allow_incomplete_var,
            self.auto_contrast_var,
        ]
        if self.gpu_build:
            preview_variables.append(self.use_gpu_var)
        for variable in preview_variables:
            variable.trace_add("write", self._preview_option_changed)
        self._update_resolution_label()
        self._update_jpeg_quality_label()
        self._format_changed()
        self._refresh_sessions()
        self.root.after(100, self._drain_events)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    @staticmethod
    def _settings_path() -> Path:
        app_data = os.environ.get("APPDATA")
        base = Path(app_data) if app_data else Path.home() / ".config"
        return base / "PanoramaCapture" / "gui-settings.json"

    @classmethod
    def _log_path(cls) -> Path:
        return cls._settings_path().with_name("stitcher.log")

    def _load_settings(self) -> None:
        try:
            with self._settings_path().open(encoding="utf-8") as stream:
                settings = ApplicationSettings.from_mapping(json.load(stream))
            self.game_dir_var.set(self._display_path(settings.game_dir))
            self.image_dir_var.set(self._display_path(settings.image_dir))
            self.output_dir_var.set(self._display_path(settings.output_dir))
            self.auto_contrast_var.set(settings.auto_contrast)
            self._history = dict(settings.stitched_sessions)
        except (OSError, ValueError):
            pass

    def _save_settings(self) -> None:
        path = self._settings_path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_suffix(".partial")
            temporary.write_text(
                json.dumps(
                    ApplicationSettings(
                        self.game_dir_var.get(),
                        self.image_dir_var.get(),
                        self.output_dir_var.get(),
                        self._history,
                        self.auto_contrast_var.get(),
                    ).to_mapping(),
                    indent=2,
                ),
                encoding="utf-8",
            )
            os.replace(temporary, path)
        except OSError:
            pass

    def _set_path(self, variable: tk.StringVar, value: Any) -> None:
        """Store a path using the same native display form everywhere."""
        variable.set(self._display_path(value))

    def _close(self) -> None:
        if self._state is UiState.CLOSING:
            if self._worker is None or not self._worker.is_alive():
                self._finish_close()
            return
        if self._state is UiState.BUSY:
            self._state = UiState.CLOSING
            self._close_when_idle = True
            self.cancel()
            self.status_var.set("Cancellation requested; closing when the worker stops…")
            return
        self._state = UiState.CLOSING
        self._finish_close()

    def _finish_close(self) -> None:
        if self._close_finished:
            return
        self._close_finished = True
        self._close_gpu_preview_display()
        self._gpu_session_cache.close()
        self._save_settings()
        self.root.destroy()

    @staticmethod
    def _display_path(value: Any) -> str:
        raw = str(value)
        if os.name != "nt":
            return raw
        if raw.startswith("/mnt/") and len(raw) > 6:
            converted = raw[7:].replace("/", "\\")
            return f"{raw[5].upper()}:\\{converted}"
        return raw.replace("/", "\\")

    def _build_variables(self) -> None:
        self.session_var = tk.StringVar()
        self.game_dir_var = tk.StringVar()
        self.image_dir_var = tk.StringVar()
        self.output_dir_var = tk.StringVar()
        self.output_name_var = tk.StringVar(value="panorama.jpg")
        self._output_name_dirty = False
        self._advanced_visible = False
        self.format_var = tk.StringVar(value="JPEG")
        self.jpeg_quality_var = tk.IntVar(value=95)
        self.jpeg_quality_label_var = tk.StringVar()
        self.width_var = tk.StringVar()
        self.resolution_percent_var = tk.IntVar(value=100)
        self.resolution_label_var = tk.StringVar()
        self.session_thumbnail_var = tk.BooleanVar(value=False)
        self.blend_var = tk.StringVar(value="feather")
        self.memory_var = tk.StringVar(value="1024")
        self.workers_var = tk.StringVar(value="Auto")
        self.allow_incomplete_var = tk.BooleanVar(value=False)
        self.coverage_var = tk.BooleanVar(value=False)
        self.auto_contrast_var = tk.BooleanVar(value=True)
        self.overlay_var = tk.BooleanVar(value=False)
        self.use_gpu_var = tk.BooleanVar(value=self.gpu_build)
        self.backend_var = tk.StringVar(value="Backend: not selected")
        self.status_var = tk.StringVar(value="Choose a game directory and session.")

    def _build_widgets(self) -> None:
        self.main_content = ttk.Frame(self.root)
        self.main_content.pack(fill="both", expand=True)
        self.main_content.columnconfigure(1, weight=1)
        self.left_column = ttk.Frame(self.main_content)
        self.left_column.grid(row=0, column=0, sticky="ns")
        self.preview_frame = ttk.LabelFrame(self.main_content, text="Preview")
        self.preview_frame.columnconfigure(0, weight=1)
        self.preview_frame.rowconfigure(0, weight=1)
        self.preview_label = ttk.Label(self.preview_frame, text="Preview will appear here")
        self.preview_label.grid(row=0, column=0, sticky="nsew", padx=12, pady=12)
        self.preview_label.bind("<Motion>", self._magnify_preview)
        self.preview_label.bind("<Leave>", self._restore_preview_overview)
        self.preview_label.bind("<Button-1>", self._preview_clicked)
        self.exposure_expand_button = ttk.Button(
            self.preview_frame,
            text="Correct exposure >>",
            command=self._toggle_exposure_panel,
        )
        self.exposure_expand_button.grid(row=1, column=0, sticky="w", padx=12, pady=(0, 12))
        self.exposure_expand_button.grid_remove()
        exposure_status_font = font.Font(
            root=self.root,
            font=font.nametofont("TkDefaultFont", root=self.root),
        )
        exposure_status_font.configure(slant="italic")
        self._exposure_status_font = exposure_status_font
        self.exposure_status_label = ttk.Label(
            self.preview_frame, text="", font=self._exposure_status_font
        )
        self.exposure_status_label.grid(row=2, column=0, sticky="w", padx=12, pady=(0, 12))
        self.exposure_status_label.grid_remove()
        self.exposure_panel = ttk.LabelFrame(self.preview_frame, text="Exposure correction")
        ttk.Checkbutton(
            self.exposure_panel,
            text="Show boundaries overlay",
            variable=self.overlay_var,
            command=self._refresh_preview_display,
        ).pack(anchor="w", padx=8, pady=6)
        self.target_button = ttk.Button(
            self.exposure_panel, text="Target exposure", command=self._toggle_target_mode
        )
        self.target_button.pack(anchor="w", padx=8, pady=4)
        self.pose_grid = ttk.Frame(self.exposure_panel)
        self.pose_grid.pack(fill="x", padx=8, pady=6)
        exposure_actions = ttk.Frame(self.exposure_panel)
        exposure_actions.pack(fill="x", padx=8, pady=6)
        self.match_exposure_button = ttk.Button(
            exposure_actions,
            text="Match exposure",
            command=self._match_exposure,
            state="disabled",
        )
        self.match_exposure_button.pack(side="left")
        self.automatic_exposure_button = ttk.Button(
            exposure_actions,
            text="Automatic correction",
            command=self._correct_exposure_automatically,
            state="disabled",
        )
        self.automatic_exposure_button.pack(side="left", padx=(8, 0))
        self.discard_exposure_button = ttk.Button(
            exposure_actions, text="Discard changes", command=self._discard_exposure_changes
        )
        paths = ttk.LabelFrame(self.left_column, text="Capture")
        paths.pack(fill="x", padx=12, pady=8)
        self._path_row(paths, 0, "Game directory", self.game_dir_var, self._pick_game_dir)
        self.sessions_tree = ttk.Treeview(
            paths, columns=("date", "status", "stitched"), show="headings", height=6
        )
        for column, heading, width in (
            ("date", "Local date", 180),
            ("status", "Status", 120),
            ("stitched", "Stitched", 100),
        ):
            self.sessions_tree.heading(column, text=heading)
            self.sessions_tree.column(column, width=width, anchor="w")
        self.sessions_tree.grid(row=1, column=1, columnspan=2, sticky="ew", padx=6, pady=4)
        self.sessions_tree.bind("<<TreeviewSelect>>", self._session_selected)
        ttk.Button(paths, text="Refresh", command=self._refresh_sessions).grid(
            row=1, column=0, padx=6, pady=4, sticky="w"
        )
        self.delete_json_button = ttk.Button(
            paths,
            text="Delete JSON",
            command=lambda: self._delete_selected(False),
            state="disabled",
        )
        self.delete_json_button.grid(row=2, column=0, padx=6, pady=4, sticky="w")
        self.delete_sources_button = ttk.Button(
            paths,
            text="Delete JSON and screenshots",
            command=lambda: self._delete_selected(True),
            state="disabled",
        )
        self.delete_sources_button.grid(row=2, column=1, columnspan=2, padx=6, pady=4, sticky="w")
        self._path_row(paths, 3, "Screenshots", self.image_dir_var, self._pick_image_dir)
        self._path_row(paths, 4, "Output directory", self.output_dir_var, self._pick_output_dir)
        self._path_row(paths, 5, "Output filename", self.output_name_var, None)

        options = ttk.LabelFrame(self.left_column, text="Render options")
        options.pack(fill="x", padx=12, pady=8)
        self._option_row(
            options,
            0,
            "Format",
            ttk.Combobox(
                options,
                textvariable=self.format_var,
                values=("PNG", "JPEG", "EXR"),
                state="readonly",
                width=12,
            ),
        )
        jpeg_quality = ttk.Frame(options)
        self.jpeg_quality_scale = tk.Scale(
            jpeg_quality,
            from_=1,
            to=100,
            orient="horizontal",
            variable=self.jpeg_quality_var,
            showvalue=False,
            length=260,
            command=lambda _value: self._update_jpeg_quality_label(),
        )
        self.jpeg_quality_scale.pack(side="left")
        ttk.Label(jpeg_quality, textvariable=self.jpeg_quality_label_var, width=6).pack(side="left")
        self.jpeg_quality_label = ttk.Label(options, text="JPEG quality", width=24)
        self.jpeg_quality_row = jpeg_quality
        resolution = ttk.Frame(options)
        self.resolution_scale = tk.Scale(
            resolution,
            from_=1,
            to=100,
            orient="horizontal",
            variable=self.resolution_percent_var,
            showvalue=False,
            length=260,
            command=lambda _value: self._update_resolution_label(),
        )
        self.resolution_scale.pack(side="left")
        ttk.Label(resolution, textvariable=self.resolution_label_var, width=6).pack(side="left")
        self._option_row(options, 2, "Resolution", resolution)
        muted_style = ttk.Style(self.root)
        muted_style.configure("Muted.TLabel", foreground="#777777")
        self.expected_resolution_var = tk.StringVar()
        ttk.Label(
            options,
            textvariable=self.expected_resolution_var,
            style="Muted.TLabel",
        ).grid(row=3, column=0, sticky="w", padx=6, pady=3)
        ttk.Checkbutton(
            options, text="Generate session thumbnail", variable=self.session_thumbnail_var
        ).grid(row=3, column=1, sticky="w", padx=6, pady=3)
        self.advanced_button = ttk.Button(
            options, text="Advanced options ▸", command=self._toggle_advanced
        )
        self.advanced_button.grid(row=4, column=0, columnspan=2, sticky="w", padx=6, pady=4)
        self.advanced_frame = ttk.Frame(options)
        self._option_row(
            self.advanced_frame,
            0,
            "Explicit width (optional)",
            ttk.Entry(self.advanced_frame, textvariable=self.width_var, width=14),
        )
        ttk.Label(self.advanced_frame, text="Overrides the resolution slider when set.").grid(
            row=1, column=1, sticky="w", padx=6
        )
        self._option_row(
            self.advanced_frame,
            2,
            "Blend",
            ttk.Combobox(
                self.advanced_frame,
                textvariable=self.blend_var,
                values=("hard", "feather"),
                state="readonly",
                width=12,
            ),
        )
        self._option_row(
            self.advanced_frame,
            3,
            "CPU memory budget (MiB)",
            ttk.Spinbox(
                self.advanced_frame, textvariable=self.memory_var, from_=1, to=8096, width=14
            ),
        )
        ttk.Label(
            self.advanced_frame,
            text="Applies only to the CPU backend; larger budgets can render faster.",
        ).grid(row=4, column=1, sticky="w", padx=6)
        worker_choices = ("Auto", *(str(worker) for worker in range(1, (os.cpu_count() or 1) + 1)))
        self._option_row(
            self.advanced_frame,
            5,
            "Workers",
            ttk.Combobox(
                self.advanced_frame,
                textvariable=self.workers_var,
                values=worker_choices,
                state="readonly",
                width=12,
            ),
        )
        ttk.Checkbutton(
            self.advanced_frame, text="Allow incomplete session", variable=self.allow_incomplete_var
        ).grid(row=6, column=1, sticky="w", padx=6, pady=3)
        ttk.Checkbutton(
            self.advanced_frame, text="Write coverage diagnostic PNG", variable=self.coverage_var
        ).grid(row=7, column=1, sticky="w", padx=6, pady=3)
        if self.gpu_build:
            ttk.Checkbutton(
                self.advanced_frame,
                text="Use GPU acceleration when available",
                variable=self.use_gpu_var,
            ).grid(row=8, column=1, sticky="w", padx=6, pady=3)
        ttk.Checkbutton(
            self.advanced_frame, text="Auto contrast (SDR outputs)", variable=self.auto_contrast_var
        ).grid(row=9, column=1, sticky="w", padx=6, pady=3)

        actions = ttk.Frame(self.left_column)
        actions.pack(fill="x", padx=12, pady=8)
        self.render_button = ttk.Button(
            actions, text="Preview", command=self.render, state="disabled"
        )
        self.render_button.pack(side="left", padx=8)
        self.discard_button = ttk.Button(
            actions, text="Discard preview", command=self.discard_preview
        )
        self.cancel_button = ttk.Button(
            actions, text="Cancel", command=self.cancel, state="disabled"
        )
        self.cancel_button.pack(side="left")

        self.progress = ttk.Progressbar(self.left_column, mode="determinate", maximum=100)
        self.progress.pack(fill="x", padx=12, pady=4)
        if self.gpu_build:
            ttk.Label(self.left_column, textvariable=self.backend_var).pack(
                fill="x", padx=12, pady=(4, 0)
            )
        ttk.Label(self.left_column, textvariable=self.status_var, wraplength=640).pack(
            fill="x", padx=12, pady=8
        )

    @staticmethod
    def _path_row(
        parent: ttk.LabelFrame,
        row: int,
        label: str,
        variable: tk.StringVar,
        browse: Any,
    ) -> None:
        ttk.Label(parent, text=label, width=20).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(parent, textvariable=variable).grid(
            row=row, column=1, sticky="ew", padx=6, pady=4
        )
        if browse is not None:
            ttk.Button(parent, text="Browse…", command=browse).grid(
                row=row, column=2, padx=6, pady=4
            )
        parent.columnconfigure(1, weight=1)

    @staticmethod
    def _option_row(parent: Any, row: int, label: str, widget: Any) -> None:
        ttk.Label(parent, text=label, width=24).grid(row=row, column=0, sticky="w", padx=6, pady=3)
        widget.grid(row=row, column=1, sticky="w", padx=6, pady=3)

    def _pick_game_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select Cyberpunk 2077 game directory", initialdir=self.game_dir_var.get() or None
        )
        if chosen:
            self._set_path(self.game_dir_var, chosen)
            self._refresh_sessions()

    def _pick_image_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select screenshots directory",
            initialdir=self.image_dir_var.get() or self.game_dir_var.get() or None,
        )
        if chosen:
            self._set_path(self.image_dir_var, chosen)

    def _refresh_sessions(self) -> None:
        game_text = self.game_dir_var.get().strip()
        self._sessions = discover_sessions(Path(game_text), self._history) if game_text else ()
        if not hasattr(self, "sessions_tree"):
            return
        self.sessions_tree.delete(*self.sessions_tree.get_children())
        for index, record in enumerate(self._sessions):
            status = (
                "Invalid" if record.error else ("Complete" if record.complete else "Incomplete")
            )
            stitched = "Yes" if record.stitched_name else "No"
            self.sessions_tree.insert(
                "", "end", iid=str(index), values=(record.local_date, status, stitched)
            )
        self._validated = False
        self.render_button.configure(state="disabled")
        self.delete_json_button.configure(state="disabled")
        self.delete_sources_button.configure(state="disabled")

    def _session_selected(self, _event: object) -> None:
        selected = self.sessions_tree.selection()
        if not selected:
            return
        record = self._sessions[int(selected[0])]
        if record.error:
            self.status_var.set(f"Cannot load {record.path.name}: {record.error}")
            return
        self._set_path(self.session_var, record.path)
        self._set_default_output_name(record.metadata.session_id, force=True)
        if record.stitched_name:
            format_by_suffix = {".png": "PNG", ".jpg": "JPEG", ".jpeg": "JPEG", ".exr": "EXR"}
            restored_format = format_by_suffix.get(Path(record.stitched_name).suffix.lower())
            if restored_format is not None:
                self.format_var.set(restored_format)
            self.output_name_var.set(record.stitched_name)
            self._output_name_dirty = False
        inferred = self._inferred_image_directory(record.metadata, record.path.parent)
        if inferred is not None:
            self._set_path(self.image_dir_var, inferred)
        self.delete_json_button.configure(state="normal")
        self.delete_sources_button.configure(state="normal")

    def _delete_selected(self, include_images: bool) -> None:
        selected = self.sessions_tree.selection()
        if not selected:
            return
        record = self._sessions[int(selected[0])]
        if record.error:
            return
        if include_images or record.complete:
            if not messagebox.askyesno(
                "Delete session files",
                "Are you sure?\n\nThis will delete the selected session files.",
                parent=self.root,
            ):
                return
        deleted, missing = delete_files(deletion_targets(record, include_images))
        self.discard_preview()
        self.status_var.set(f"Deleted {deleted} file(s); {missing} already missing.")
        self._refresh_sessions()

    def _pick_output_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select output directory",
            initialdir=self.output_dir_var.get() or self.game_dir_var.get() or None,
        )
        if chosen:
            self._set_path(self.output_dir_var, chosen)

    def _input_paths(self) -> tuple[Path, Path]:
        session = Path(self.session_var.get()).expanduser().resolve()
        if not session.is_file():
            raise ValueError("capture JSON does not exist")
        image_text = self.image_dir_var.get().strip()
        if image_text:
            image_dir = Path(image_text).expanduser().resolve()
        else:
            loaded = load_session(session, image_directory=session.parent)
            image_dir = self._inferred_image_directory(loaded, session.parent) or session.parent
            self.image_dir_var.set(str(image_dir))
        if not image_dir.is_dir():
            raise ValueError(
                "screenshots directory does not exist; choose one or use a CET JSON "
                "with valid paths"
            )
        return session, image_dir

    def _output_directory(self) -> Path:
        output_dir = Path(self.output_dir_var.get()).expanduser().resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        if not output_dir.is_dir() or not os.access(output_dir, os.W_OK):
            raise ValueError("output directory is not writable")
        return output_dir

    @staticmethod
    def _inferred_image_directory(session: Any, fallback: Path) -> Path | None:
        candidates = [
            Path(frame.filename).parent
            for frame in session.frames
            if Path(frame.filename).is_absolute()
        ]
        return next((candidate for candidate in candidates if candidate.is_dir()), fallback)

    def _update_resolution_label(self) -> None:
        self.resolution_label_var.set(f"{self.resolution_percent_var.get()}%")
        if hasattr(self, "expected_resolution_var"):
            self._update_expected_resolution()

    def _update_expected_resolution(self) -> None:
        if self._native_output_size is None or self._resolution_geometry is None:
            self.expected_resolution_var.set("")
            return
        width_text = self.width_var.get().strip()
        try:
            width = (
                int(width_text)
                if width_text
                else max(
                    1, int(self._native_output_size[0] * self.resolution_percent_var.get() / 100)
                )
            )
        except ValueError:
            self.expected_resolution_var.set("Expected: invalid width")
            return
        mode, vertical_fov = self._resolution_geometry
        if mode == "full_sphere":
            width = max(2, width - width % 2)
            height = width // 2
        else:
            height = max(1, round(width * vertical_fov / 360.0))
        self.expected_resolution_var.set(f"Expected: {width} × {height}")

    def _update_jpeg_quality_label(self) -> None:
        self.jpeg_quality_label_var.set(f"{self.jpeg_quality_var.get()}%")

    def _toggle_advanced(self) -> None:
        self._advanced_visible = not self._advanced_visible
        if self._advanced_visible:
            self.advanced_frame.grid(row=5, column=0, columnspan=2, sticky="ew", padx=6)
            self.advanced_button.configure(text="Advanced options ▾")
        else:
            self.advanced_frame.grid_remove()
            self.advanced_button.configure(text="Advanced options ▸")

    def _output_name_changed(self, *_args: object) -> None:
        self._output_name_dirty = True

    def _preview_option_changed(self, *_args: object) -> None:
        if (
            self._state is UiState.PREVIEW
            or self._preview_image is not None
            or self._preview_report is not None
        ):
            self.discard_preview()

    def _is_output_directory_writable(self) -> bool:
        candidate = Path(self.output_dir_var.get().strip() or ".").expanduser()
        while not candidate.exists() and candidate != candidate.parent:
            candidate = candidate.parent
        return candidate.is_dir() and os.access(candidate, os.W_OK)

    def _output_directory_changed(self, *_args: object) -> None:
        self._output_dir_writable = self._is_output_directory_writable()
        self.render_button.configure(
            state="normal" if self._validated and self._output_dir_writable else "disabled"
        )
        if not self._output_dir_writable:
            self.status_var.set("Output directory is not writable.")

    def _format_changed(self, *_args: object) -> None:
        if self._preview_report is not None:
            self.discard_preview()
        if self.format_var.get() == "JPEG":
            self.jpeg_quality_label.grid(row=1, column=0, sticky="w", padx=6, pady=3)
            self.jpeg_quality_row.grid(row=1, column=1, sticky="w", padx=6, pady=3)
        else:
            self.jpeg_quality_label.grid_remove()
            self.jpeg_quality_row.grid_remove()
        if not self._output_name_dirty:
            suffix = self._suffix_for_format()
            current = self.output_name_var.get().strip() or "panorama.png"
            self.output_name_var.set(f"{Path(current).stem}{suffix}")
            self._output_name_dirty = False

    def _suffix_for_format(self) -> str:
        return {"PNG": ".png", "JPEG": ".jpg", "EXR": ".exr"}[self.format_var.get()]

    def _set_default_output_name(self, session_id: str, force: bool = False) -> None:
        if self._output_name_dirty and not force:
            return
        self.output_name_var.set(f"panorama-{session_id}{self._suffix_for_format()}")
        self._output_name_dirty = False

    def _default_output_name(self, session_id: str, suffix: str) -> str:
        return default_output_name(self.output_name_var.get(), session_id, suffix)

    def _output_path_preview(self) -> Path | None:
        session_text = self.session_var.get().strip()
        output_text = self.output_dir_var.get().strip()
        if not session_text or not output_text:
            return None
        suffix = self._suffix_for_format()
        session_path = Path(session_text).expanduser()
        try:
            session = load_session(session_path, image_directory=session_path.parent)
        except (OSError, ValueError):
            return None
        output_name = self._default_output_name(session.session_id, suffix)
        output_path = Path(output_text).expanduser() / output_name
        return output_path.with_suffix(suffix)

    def _mark_session_stitched(self, session_id: str, output_name: str) -> None:
        records = list(self._sessions)
        for index, record in enumerate(records):
            if record.metadata.session_id != session_id:
                continue
            records[index] = replace(record, stitched_name=output_name)
            iid = str(index)
            if self.sessions_tree.exists(iid):
                self.sessions_tree.set(iid, "stitched", "Yes")
            break
        self._sessions = tuple(records)

    def _input_changed(self, *_args: object) -> None:
        self._close_gpu_preview_display()
        self._gpu_session_cache.invalidate("input changed")
        if self._preview_report is not None:
            self.discard_preview()
        self._validated = False
        self._state = UiState.EMPTY
        self._native_output_size = None
        self._resolution_geometry = None
        if hasattr(self, "expected_resolution_var"):
            self._update_expected_resolution()
        self.render_button.configure(state="disabled")
        if self._validation_after is not None:
            self.root.after_cancel(self._validation_after)
        if self.session_var.get().strip() and self.image_dir_var.get().strip():
            self._validation_after = self.root.after(300, self._start_validation)
        else:
            self.status_var.set("Choose a game directory and session.")

    def _start_validation(self) -> None:
        self._validation_after = None
        if self._worker is None or not self._worker.is_alive():
            self._start_worker("validate")

    def _start_worker(self, operation: str) -> None:
        transition = begin_operation(self._state.name.lower(), operation)
        if transition is None:
            return
        if operation not in {"validate", "render"}:
            self._close_gpu_preview_display()
        self._state_before_busy = UiState[transition.previous_state.upper()]
        self._state = UiState.BUSY
        self._active_operation = operation
        self._set_busy(True)
        self._cancel_event = threading.Event()
        self._worker = threading.Thread(target=self._worker_main, args=(operation,), daemon=True)
        self._worker.start()

    def _emit_event(self, kind: str, payload: Any) -> None:
        self._events.put((kind, payload))

    def _requested_render_operation(self) -> str | None:
        operation = requested_render_operation(self._state.name.lower())
        return operation.value if operation is not None else None

    def render(self) -> None:
        operation = self._requested_render_operation()
        if not self._validated or not self._output_dir_writable or operation is None:
            return
        if operation == "preview":
            self.root.update_idletasks()
            self._preview_width = max(1, self.root.winfo_width() - 24)
            self.preview_frame.grid(row=0, column=1, sticky="nsew", padx=12, pady=8)
            self._start_worker("preview")
            return
        output_path = self._output_path_preview()
        output_plan = (
            plan_outputs(
                output_path.parent,
                output_path.name,
                output_path.suffix,
                coverage=self.coverage_var.get(),
                thumbnail=self.session_thumbnail_var.get(),
            )
            if output_path is not None
            else None
        )
        existing = output_plan.existing() if output_plan is not None else ()
        if existing:
            if not messagebox.askyesno(
                "Overwrite existing file?",
                "The following output files already exist:\n"
                + "\n".join(path.name for path in existing)
                + "\nReplace them?",
                parent=self.root,
            ):
                return
        self._start_worker(operation)

    def discard_preview(self) -> None:
        self._close_gpu_preview_display()
        self._gpu_session_cache.invalidate("preview discarded")
        self._preview_report = None
        self._preview_photo = None
        self._preview_magnified_photo = None
        self._preview_image = None
        self._preview_overview = None
        self._preview_session = None
        self._coverage_masks = ()
        self._active_preview_crop = None
        with self._display_lock:
            self._display_generation += 1
            self._display_valid_generation = self._display_generation
            self._display_applied_generation = self._display_generation
            self._display_pending = None
        self._manual_gains = ()
        if hasattr(self, "_selected_poses"):
            self._selected_poses.clear()
        self._target_pose = None
        if hasattr(self, "_hovered_poses"):
            self._hovered_poses.clear()
        self._target_mode = False
        if hasattr(self, "overlay_var"):
            self.overlay_var.set(False)
        if hasattr(self, "discard_exposure_button"):
            self.discard_exposure_button.pack_forget()
        if hasattr(self, "pose_grid"):
            for child in self.pose_grid.winfo_children():
                if isinstance(child, tk.Widget):
                    child.grid_remove()
            self.exposure_panel.grid_remove()
            self.exposure_expand_button.grid_remove()
            self.exposure_expand_button.configure(text="Correct exposure >>")
            self.exposure_status_label.configure(text="")
            self.exposure_status_label.grid_remove()
            self.target_button.configure(text="Target exposure")
            self.preview_label.configure(cursor="")
        if hasattr(self, "preview_label"):
            self.preview_label.configure(image="", text="Preview will appear here")
            self.preview_frame.grid_remove()
        self.render_button.configure(text="Preview")
        self.discard_button.pack_forget()
        self._state = UiState.READY if self._validated else UiState.EMPTY

    def _toggle_exposure_panel(self) -> None:
        if self.exposure_panel.winfo_ismapped():
            self.exposure_panel.grid_remove()
            self.exposure_expand_button.configure(text="Correct exposure >>")
        else:
            self.exposure_panel.grid(row=0, column=1, rowspan=3, sticky="ns", padx=(0, 12), pady=12)
            self.exposure_expand_button.configure(text="Correct exposure <<")

    def _toggle_target_mode(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        self._target_mode = not self._target_mode
        self.target_button.configure(
            text="Target exposure (selecting)" if self._target_mode else "Target exposure"
        )
        try:
            self.preview_label.configure(cursor="target" if self._target_mode else "")
        except tk.TclError:
            self.preview_label.configure(cursor="crosshair" if self._target_mode else "")
        self.status_var.set(
            "Select the target exposure pose."
            if self._target_mode
            else "Select poses whose exposure should be shifted."
        )
        self._refresh_pose_grid()
        self._refresh_preview_display()

    def _finish_target_selection(self) -> None:
        self._target_mode = False
        self.target_button.configure(text="Target exposure")
        self.preview_label.configure(cursor="")
        self.status_var.set("Select poses whose exposure should be shifted.")

    def _build_pose_grid(self) -> None:
        if self._preview_session is None:
            return
        columns = 8
        for position, frame in enumerate(self._preview_session.frames):
            if position < len(self._pose_widgets):
                label = self._pose_widgets[position]
                label.configure(text=str(frame.index))
            else:
                label = tk.Label(
                    self.pose_grid,
                    text=str(frame.index),
                    width=3,
                    height=1,
                    bd=0,
                    highlightthickness=1,
                )
                label.bind("<Enter>", partial(self._pose_entered, position))
                label.bind("<Leave>", partial(self._pose_left, position))
                label.bind("<Button-1>", partial(self._pose_clicked, position))
                self._pose_widgets.append(label)
            label.grid(row=position // columns, column=position % columns, padx=2, pady=2)
        for label in self._pose_widgets[len(self._preview_session.frames) :]:
            label.grid_remove()
        self._refresh_pose_grid()

    def _pose_entered(self, position: int, _event: Any = None) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        self._hovered_poses = {position}
        self._refresh_pose_grid()
        self._refresh_preview_display()

    def _pose_left(self, position: int, _event: Any = None) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        self._hovered_poses.discard(position)
        self._refresh_pose_grid()
        self._refresh_preview_display()

    def _pose_clicked(self, position: int, _event: Any = None) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        if self._target_mode:
            if position in self._selected_poses:
                return
            self._target_pose = position
            self._finish_target_selection()
        else:
            if position == self._target_pose:
                return
            if position in self._selected_poses:
                self._selected_poses.remove(position)
            else:
                self._selected_poses.add(position)
        self._refresh_pose_grid()
        self._refresh_preview_display()

    def _preview_clicked(self, event: Any) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        if self._preview_image is None or not self.overlay_var.get():
            return
        candidates = self._preview_pose_candidates(event, self._active_preview_crop)
        if self._target_mode:
            if candidates:
                self._target_pose = candidates[0]
                self._finish_target_selection()
        else:
            for position in candidates:
                if position in self._selected_poses:
                    self._selected_poses.remove(position)
                else:
                    self._selected_poses.add(position)
        self._refresh_pose_grid()
        self._refresh_preview_display()

    def _preview_pose_candidates(
        self, event: Any, crop_box: tuple[int, int, int, int] | None
    ) -> list[int]:
        if self._preview_image is None:
            return []
        viewport_width, viewport_height = self._preview_viewport
        image_left = (self.preview_label.winfo_width() - viewport_width) / 2
        image_top = (self.preview_label.winfo_height() - viewport_height) / 2
        crop = crop_box or (
            0,
            0,
            self._preview_image.width,
            self._preview_image.height,
        )
        crop_width = crop[2] - crop[0]
        crop_height = crop[3] - crop[1]
        source_x = crop[0] + min(
            crop_width - 1,
            max(0, int((event.x - image_left) * crop_width / viewport_width)),
        )
        source_y = crop[1] + min(
            crop_height - 1,
            max(0, int((event.y - image_top) * crop_height / viewport_height)),
        )
        candidates = [
            position
            for position, mask in enumerate(self._coverage_masks)
            if bool(
                mask[
                    min(mask.shape[0] - 1, source_y * mask.shape[0] // self._preview_image.height),
                    min(mask.shape[1] - 1, source_x * mask.shape[1] // self._preview_image.width),
                ]
            )
        ]
        if self._target_mode:
            candidates = [
                position for position in candidates if position not in self._selected_poses
            ]
            return candidates[:1]
        return [position for position in candidates if position != self._target_pose]

    def _refresh_pose_grid(self) -> None:
        for position, widget in enumerate(self._pose_widgets):
            if position == self._target_pose:
                widget.configure(
                    bg="#a8cfff", fg="black", relief="solid", highlightbackground="blue"
                )
            elif position in self._selected_poses:
                border = (
                    "blue"
                    if position in self._hovered_poses and self._target_mode
                    else "magenta"
                    if position in self._hovered_poses
                    else "green"
                )
                widget.configure(
                    bg="#91bc91", fg="black", relief="solid", highlightbackground=border
                )
            elif position in self._hovered_poses:
                color = "blue" if self._target_mode else "magenta"
                widget.configure(bg="#d9d9d9", fg=color, relief="solid", highlightbackground=color)
            else:
                widget.configure(
                    bg="#d9d9d9", fg="black", relief="solid", highlightbackground="gray"
                )
        state = "normal" if self._target_pose is not None and self._selected_poses else "disabled"
        self.match_exposure_button.configure(state=state)
        if hasattr(self, "automatic_exposure_button"):
            self.automatic_exposure_button.configure(
                state="normal" if self._target_pose is not None else "disabled"
            )

    def _match_exposure(self) -> None:
        if self._target_pose is not None and self._selected_poses:
            self._start_worker("match_exposure")

    def _correct_exposure_automatically(self) -> None:
        if self._target_pose is not None:
            self._start_worker("automatic_exposure")

    def _discard_exposure_changes(self) -> None:
        if self._manual_gains:
            self._manual_gains = ExposureEdits(self._manual_gains).discard().gains
            self.discard_exposure_button.pack_forget()
            self.exposure_status_label.configure(text="")
            self.exposure_status_label.grid_remove()
            self._start_worker("preview")

    def cancel(self) -> None:
        if cancellation_requested(self._cancel_event is not None):
            assert self._cancel_event is not None
            self._cancel_event.set()
            self.status_var.set("Cancellation requested…")

    def _worker_main(self, operation: str) -> None:
        try:
            LOGGER.info("%s started", operation)
            self._emit_event("backend", ("selecting", "checking GPU availability and VRAM"))
            session_path, image_dir = self._input_paths()
            allow_incomplete = self.allow_incomplete_var.get()
            session = validate_session_request(
                session_path, image_dir, allow_incomplete, self._cancel_event
            )
            if operation == "validate":
                if self._cancel_event is not None and self._cancel_event.is_set():
                    raise RenderCancelledError("render cancelled")
                native = estimate_render_resources(session, image_dir, None, 1024 * MIB, None)
                self._emit_event(
                    "resolution",
                    (
                        (native.output_width, native.output_height),
                        (session.capture_mode.value, session.vertical_fov_deg),
                    ),
                )
                self._emit_event("validated", f"Valid session: {session.session_id}")
                return
            output_dir = self._output_directory()
            manual_gains = (
                self._manual_gains
                if len(self._manual_gains) == len(session.frames)
                else (1.0,) * len(session.frames)
            )
            automatic_corrected_poses: tuple[int, ...] = ()
            if operation == "match_exposure":
                if self._target_pose is None or not self._selected_poses:
                    raise ValueError("select a target exposure and at least one pose to shift")
                gain = estimate_target_exposure_gain(
                    session,
                    image_dir,
                    self._target_pose,
                    tuple(sorted(self._selected_poses)),
                    manual_gains,
                    self._cancel_event,
                    lambda completed, total, phase: self._progress(
                        completed, total, f"Exposure match: {phase}"
                    ),
                    match_proxies=(
                        self._preview_report.match_proxies
                        if self._preview_report is not None
                        else ()
                    ),
                )
                manual_gains = (
                    ExposureEdits(
                        manual_gains,
                        self._target_pose,
                        frozenset(self._selected_poses),
                    )
                    .apply_match(gain)
                    .gains
                )
            elif operation == "automatic_exposure":
                if self._target_pose is None:
                    raise ValueError("select a target exposure pose")
                automatic = estimate_automatic_exposure_gains(
                    session,
                    image_dir,
                    self._target_pose,
                    manual_gains,
                    self._cancel_event,
                    lambda completed, total, phase: self._progress(
                        completed, total, f"Automatic exposure: {phase}"
                    ),
                    match_proxies=(
                        self._preview_report.match_proxies
                        if self._preview_report is not None
                        else ()
                    ),
                )
                manual_gains = tuple(
                    current * correction
                    for current, correction in zip(manual_gains, automatic.gains, strict=True)
                )
                automatic_corrected_poses = tuple(
                    position + 1 for position in automatic.corrected_positions
                )
            scale = Fraction(self.resolution_percent_var.get(), 100)
            if scale <= 0 or scale > 1:
                raise ValueError("resolution must be between 1/1 and a positive fraction")
            width = int(self.width_var.get()) if self.width_var.get().strip() else None
            if width is not None and width < 1:
                raise ValueError("explicit width must be positive")
            memory = int(self.memory_var.get()) * 1024 * 1024
            if memory < 1 * 1024 * 1024 or memory > 8096 * 1024 * 1024:
                raise ValueError("memory budget must be between 1 and 8096 MiB")
            workers = None if self.workers_var.get() == "Auto" else int(self.workers_var.get())
            render_width = width
            if render_width is None and scale != 1:
                full = estimate_render_resources(session, image_dir, None, memory, workers)
                render_width = max(1, int(full.output_width * scale))
            suffix = self._suffix_for_format()
            output_name = self._default_output_name(session.session_id, suffix)
            output_path = output_dir / output_name
            if output_path.suffix.lower() != suffix:
                output_path = output_path.with_suffix(suffix)
            coverage = (
                output_path.with_name(f"{output_path.stem}-coverage.png")
                if self.coverage_var.get()
                else None
            )
            resources = estimate_render_resources(session, image_dir, render_width, memory, workers)
            if operation in {"preview", "match_exposure", "automatic_exposure"}:
                preview = render_preview(
                    session,
                    image_dir,
                    self._preview_width,
                    suffix,
                    blend=self.blend_var.get(),
                    allow_incomplete=allow_incomplete,
                    memory_budget_bytes=memory,
                    progress_callback=lambda completed, total, phase: self._progress(
                        completed, total, f"Preview: {phase}"
                    ),
                    cancel_event=self._cancel_event,
                    workers=workers,
                    auto_contrast=self.auto_contrast_var.get(),
                    use_gpu=self.use_gpu_var.get(),
                    backend_callback=self._backend_selected,
                    gpu_session_cache=self._gpu_session_cache,
                    gpu_session_path=session_path,
                    gpu_width_multiplier=GPU_PREVIEW_WIDTH_MULTIPLIER,
                    manual_gains=manual_gains,
                    exposure_report=self._preview_report,
                )
                mask_height = max(
                    1,
                    round(self._preview_width * preview.pixels.shape[0] / preview.pixels.shape[1]),
                )
                masks = frame_coverage_masks(
                    session, image_dir, self._preview_width, mask_height, self._cancel_event
                )
                overview_image = Image.fromarray(preview.pixels, mode="RGB").resize(
                    (self._preview_width, mask_height), Image.Resampling.LANCZOS
                )
                overview = np.asarray(overview_image, dtype=np.uint8)
                gpu_display = self._gpu_session_cache.create_preview_display(
                    preview.pixels, overview, masks
                )
                self._emit_event(
                    "preview", (preview, overview, masks, session, manual_gains, gpu_display)
                )
                if operation == "automatic_exposure":
                    self._emit_event("automatic_exposure_applied", automatic_corrected_poses)
                return
            self._emit_event(
                "status",
                f"Rendering {resources.output_width}×{resources.output_height} "
                f"with {resources.worker_count} workers…",
            )
            render_session(
                session,
                image_dir,
                output_path,
                render_width,
                self.blend_var.get(),
                allow_incomplete,
                memory,
                self._progress,
                coverage,
                self._cancel_event,
                self.jpeg_quality_var.get(),
                workers,
                self.auto_contrast_var.get(),
                session_thumbnail=self.session_thumbnail_var.get(),
                exposure_report=self._preview_report,
                use_gpu=self.use_gpu_var.get(),
                backend_callback=self._backend_selected,
                gpu_session_cache=self._gpu_session_cache,
                gpu_session_path=session_path,
                manual_gains=manual_gains,
            )
            LOGGER.info("render completed: %s", output_path)
            self._gpu_session_cache.invalidate("render completed")
            self._emit_event("stitched", (session.session_id, output_path.name))
            auto_state = (
                "skipped for EXR"
                if output_path.suffix.lower() == ".exr"
                else ("enabled" if self.auto_contrast_var.get() else "disabled")
            )
            thumbnail_note = (
                f"; thumbnail: {thumbnail_output_path(output_path)}"
                if self.session_thumbnail_var.get()
                else ""
            )
            self._emit_event(
                "success",
                f"Wrote {output_path} (auto contrast: {auto_state}{thumbnail_note})",
            )
        except RenderCancelledError:
            self._gpu_session_cache.invalidate("render cancelled")
            LOGGER.info("%s cancelled", operation)
            self._emit_event("cancelled", "Render cancelled; partial files were removed.")
        except AutomaticExposureAmbiguousError as error:
            LOGGER.info("automatic exposure correction not applied: %s", error)
            self._emit_event("automatic_exposure_warning", str(error))
        except Exception as error:
            self._gpu_session_cache.invalidate("render failed")
            LOGGER.exception("%s failed", operation)
            kind = (
                "match_error"
                if operation == "match_exposure"
                else ("render_error" if operation == "render" else "error")
            )
            self._emit_event(kind, f"{error}\n\nDetails were written to {self._log_path()}")
        finally:
            self._emit_event("idle", "")

    def _progress(self, completed: int, total: int, phase: str) -> None:
        self._emit_event("progress", (completed, total, phase))

    def _backend_selected(self, backend: str, detail: str) -> None:
        self._emit_event("backend", (backend, detail))

    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self._events.get_nowait()
                if kind == "progress":
                    completed, total, phase = payload
                    self.progress["value"] = completed * 100 / total if total else 0
                    self.status_var.set(f"{phase}: {completed}/{total}")
                elif kind == "status":
                    self.status_var.set(payload)
                elif kind == "resolution":
                    self._native_output_size, self._resolution_geometry = payload
                    self._update_expected_resolution()
                elif kind == "backend":
                    backend, detail = payload
                    self.backend_var.set(
                        _backend_status(
                            backend,
                            detail,
                            gpu_requested=self.use_gpu_var.get(),
                            log_directory=self._log_path().parent,
                        )
                    )
                elif kind == "preview":
                    preview, overview, masks, session, manual_gains, gpu_display = payload
                    self._preview_image = Image.fromarray(preview.pixels, mode="RGB")
                    self._preview_overview = Image.fromarray(overview, mode="RGB")
                    self._gpu_preview_display = gpu_display
                    overview_height = max(
                        1,
                        round(
                            self._preview_width
                            * self._preview_image.height
                            / self._preview_image.width
                        ),
                    )
                    self._preview_viewport = (self._preview_width, overview_height)
                    self._preview_session = session
                    self._coverage_masks = masks
                    self._manual_gains = manual_gains
                    self._build_pose_grid()
                    self._refresh_preview_display()
                    self.exposure_expand_button.grid()
                    self._preview_report = preview.exposure_report
                    self._state = UiState.PREVIEW
                    self.render_button.configure(text="Render full size")
                    self.discard_button.pack(side="left", padx=8)
                    if any(abs(gain - 1.0) > 1e-6 for gain in manual_gains):
                        self.discard_exposure_button.pack(side="left", padx=8)
                        self.exposure_status_label.configure(
                            text="Exposure corrections will be used for final output."
                        )
                        self.exposure_status_label.grid()
                    else:
                        self.discard_exposure_button.pack_forget()
                        self.exposure_status_label.configure(text="")
                        self.exposure_status_label.grid_remove()
                    self.status_var.set("Preview ready. Render full size or discard it.")
                elif kind == "preview_display":
                    generation, display, crop_box = payload
                    if (
                        generation < self._display_valid_generation
                        or generation <= self._display_applied_generation
                        or self._preview_image is None
                    ):
                        continue
                    self._display_applied_generation = generation
                    photo = ImageTk.PhotoImage(display)
                    if crop_box is None:
                        self._preview_photo = photo
                    else:
                        self._preview_magnified_photo = photo
                    self.preview_label.configure(image=photo, text="")
                    if crop_box is None:
                        self._expand_window_for_preview()
                elif kind == "automatic_exposure_applied":
                    if payload:
                        poses = ", ".join(map(str, payload))
                        self.status_var.set(f"Automatically corrected exposure for poses: {poses}.")
                    else:
                        self.status_var.set(
                            "Pose exposures already agree; no correction was needed."
                        )
                elif kind == "automatic_exposure_warning":
                    self.status_var.set(f"Automatic exposure correction skipped: {payload}")
                    messagebox.showwarning(
                        "Automatic exposure correction",
                        f"{payload}\n\nSelect poses manually to correct their exposure.",
                        parent=self.root,
                    )
                elif kind == "validated":
                    self._validated = True
                    self._state = UiState.READY
                    self.render_button.configure(
                        state="normal" if self._output_dir_writable else "disabled"
                    )
                    self.status_var.set(
                        payload
                        if self._output_dir_writable
                        else "Output directory is not writable."
                    )
                elif kind == "success":
                    self._state = UiState.PREVIEW
                    self.status_var.set(payload)
                    messagebox.showinfo("Panorama stitcher", payload, parent=self.root)
                elif kind == "stitched":
                    session_id, output_name = payload
                    mark_stitched(
                        self._history, Path(self.game_dir_var.get()), session_id, output_name
                    )
                    self._mark_session_stitched(session_id, output_name)
                    self._save_settings()
                elif kind == "error":
                    self.discard_preview()
                    self._active_operation = None
                    self._set_busy(False)
                    self.status_var.set(f"Error: {payload}")
                    messagebox.showerror("Panorama stitcher", payload, parent=self.root)
                elif kind == "render_error":
                    self._active_operation = None
                    self._set_busy(False)
                    self.discard_preview()
                    self.progress["value"] = 0
                    self.status_var.set(f"Render failed: {payload}")
                    messagebox.showerror("Panorama stitcher", payload, parent=self.root)
                elif kind == "match_error":
                    self.status_var.set(f"Exposure match failed: {payload}")
                    messagebox.showerror("Exposure match failed", payload, parent=self.root)
                elif kind == "cancelled":
                    self.discard_preview()
                    self.status_var.set(payload)
                elif kind == "idle":
                    restored = finish_operation(
                        self._state.name.lower(), self._state_before_busy.name.lower()
                    )
                    self._state = UiState[restored.upper()]
                    self._active_operation = None
                    self._set_busy(False)
                    if self._close_when_idle:
                        self.root.after(50, self._close)
        except queue.Empty:
            pass
        self.root.after(100, self._drain_events)

    def _magnify_preview(self, event: Any) -> None:
        if self._preview_image is None or (self._worker is not None and self._worker.is_alive()):
            return
        viewport_width, viewport_height = self._preview_viewport
        image_left = (self.preview_label.winfo_width() - viewport_width) / 2
        image_top = (self.preview_label.winfo_height() - viewport_height) / 2
        pointer = (
            (event.x - image_left) / viewport_width,
            (event.y - image_top) / viewport_height,
        )
        box = _magnified_crop_box(self._preview_image.size, self._preview_viewport, pointer)
        self._hovered_poses = (
            set(self._preview_pose_candidates(event, box)) if self.overlay_var.get() else set()
        )
        self._refresh_pose_grid()
        self._refresh_preview_display(box)

    def _restore_preview_overview(self, _event: Any = None) -> None:
        self._preview_magnified_photo = None
        self._hovered_poses.clear()
        self._refresh_pose_grid()
        if self._preview_photo is not None:
            self.preview_label.configure(image=self._preview_photo, text="")
        self._refresh_preview_display()

    def _expand_window_for_preview(self) -> None:
        """Grow the window to its preview request without overriding a larger user size."""

        self.root.update_idletasks()
        width = max(self.root.winfo_width(), self.main_content.winfo_reqwidth())
        height = max(self.root.winfo_height(), self.main_content.winfo_reqheight())
        self.root.geometry(f"{width}x{height}")

    def _refresh_preview_display(self, crop_box: tuple[int, int, int, int] | None = None) -> None:
        if self._preview_image is None:
            return
        self._active_preview_crop = crop_box
        overlay = self.overlay_var.get()
        masks = self._coverage_masks if overlay or self._hovered_poses else ()
        with self._display_lock:
            self._display_generation += 1
            self._display_pending = (
                self._display_generation,
                self._preview_image,
                self._preview_overview,
                self._gpu_preview_display,
                self._preview_viewport,
                masks,
                overlay,
                crop_box,
                frozenset(self._hovered_poses),
                self._target_pose,
                self._target_mode,
            )
            if self._display_worker is not None and self._display_worker.is_alive():
                return
            self._display_worker = threading.Thread(
                target=self._preview_display_worker_main, daemon=True
            )
            self._display_worker.start()

    def _preview_display_worker_main(self) -> None:
        while True:
            with self._display_lock:
                request = self._display_pending
                self._display_pending = None
                if request is None:
                    self._display_worker = None
                    return
            (
                generation,
                source,
                overview,
                gpu_display,
                viewport,
                masks,
                overlay,
                crop_box,
                hovered,
                target,
                target_mode,
            ) = request
            if gpu_display is None:
                display_source = overview if crop_box is None and overview is not None else source
                display = _compose_preview_display(
                    display_source,
                    viewport,
                    masks,
                    overlay,
                    crop_box,
                    hovered,
                    target,
                    target_mode,
                )
            else:
                try:
                    pixels = gpu_display.render(crop_box, hovered, target, target_mode, overlay)
                except RuntimeError:
                    continue
                display = Image.fromarray(pixels, mode="RGB")
            self._emit_event("preview_display", (generation, display, crop_box))

    def _close_gpu_preview_display(self) -> None:
        with self._display_lock:
            self._display_generation += 1
            self._display_valid_generation = self._display_generation
            self._display_applied_generation = self._display_generation
            self._display_pending = None
            display = getattr(self, "_gpu_preview_display", None)
            self._gpu_preview_display = None
        if display is not None:
            display.close()

    def _form_control_widgets(self) -> list[tk.Widget]:
        controls: list[tk.Widget] = []
        pending = list(self.root.winfo_children())
        while pending:
            widget = pending.pop()
            pending.extend(widget.winfo_children())
            if isinstance(
                widget,
                (tk.Scale, ttk.Button, ttk.Checkbutton, ttk.Combobox, ttk.Entry, ttk.Spinbox),
            ):
                controls.append(widget)
        return controls

    def _set_form_enabled(self, enabled: bool) -> None:
        for widget in self._form_controls:
            if widget in {self.advanced_button, self.cancel_button, self.render_button}:
                continue
            control: Any = widget
            if isinstance(widget, ttk.Combobox):
                control.configure(state="readonly" if enabled else "disabled")
            else:
                control.configure(state="normal" if enabled else "disabled")

    def _set_busy(self, busy: bool) -> None:
        self._set_form_enabled(not busy)
        state = "disabled" if busy else "normal"
        self.render_button.configure(
            state=state if self._validated and self._output_dir_writable else "disabled"
        )
        self.cancel_button.configure(state="normal" if busy else "disabled")
        if not busy:
            self._refresh_pose_grid()


def _configure_scaling(root: tk.Tk) -> None:
    """Respect Windows display scaling when the GUI is hosted by WSLg."""

    override = os.environ.get("PANO_STITCH_GUI_SCALE")
    if override:
        scale = float(override)
    elif os.environ.get("WSL_DISTRO_NAME"):
        scale = 1.5
    else:
        return
    if scale <= 0:
        raise ValueError("PANO_STITCH_GUI_SCALE must be positive")
    root.tk.call("tk", "scaling", scale)
    for name in (
        "TkDefaultFont",
        "TkTextFont",
        "TkMenuFont",
        "TkHeadingFont",
        "TkCaptionFont",
        "TkSmallCaptionFont",
        "TkIconFont",
        "TkTooltipFont",
    ):
        named = font.nametofont(name, root=root)
        size = abs(int(named.cget("size")))
        named.configure(size=max(11, round(size * scale)))


def _configure_logging() -> None:
    log_path = StitcherApp._log_path()
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        logging.basicConfig(
            filename=log_path,
            encoding="utf-8",
            level=logging.INFO,
            format="%(asctime)s %(levelname)s %(name)s: %(message)s",
            force=True,
        )
    except OSError:
        logging.basicConfig(level=logging.INFO, force=True)
    LOGGER.info("Panorama Stitcher started; log_path=%s", log_path)


def main() -> None:
    """Launch the desktop stitcher frontend."""

    _configure_logging()
    root = tk.Tk()
    _configure_scaling(root)
    StitcherApp(root)
    root.mainloop()
