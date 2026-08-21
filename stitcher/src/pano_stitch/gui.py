"""Small desktop frontend for the metadata-driven panorama stitcher."""

from __future__ import annotations

import json
import logging
import os
import queue
import threading
import tkinter as tk  # type: ignore[import-untyped]
from fractions import Fraction
from pathlib import Path
from tkinter import filedialog, font, messagebox, ttk  # type: ignore[import-untyped]
from typing import Any

from pano_stitch.compositor import (
    RenderCancelledError,
    estimate_render_resources,
    render_session,
    renderable_session,
    validate_images,
)
from pano_stitch.metadata import load_session

LOGGER = logging.getLogger(__name__)


class StitcherApp:
    """Tkinter UI that delegates all image work to the existing stitcher API."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Cyberpunk Panorama Stitcher")
        self.root.minsize(680, 400)
        self._events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self._cancel_event: threading.Event | None = None
        self._worker: threading.Thread | None = None
        self._validation_after: str | None = None
        self._validated = False
        self._build_variables()
        self._load_settings()
        self._build_widgets()
        self._form_controls = self._form_control_widgets()
        self.session_var.trace_add("write", self._input_changed)
        self.image_dir_var.trace_add("write", self._input_changed)
        self.output_name_var.trace_add("write", self._output_name_changed)
        self.format_var.trace_add("write", self._format_changed)
        self._update_resolution_label()
        self._update_jpeg_quality_label()
        self._format_changed()
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
                settings = json.load(stream)
            self.session_dir_var.set(self._display_path(settings.get("session_dir", "")))
            self.image_dir_var.set(self._display_path(settings.get("image_dir", "")))
            self.output_dir_var.set(self._display_path(settings.get("output_dir", "")))
        except (OSError, ValueError):
            pass

    def _save_settings(self) -> None:
        path = self._settings_path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_suffix(".partial")
            temporary.write_text(
                json.dumps(
                    {
                        "session_dir": self.session_dir_var.get(),
                        "image_dir": self.image_dir_var.get(),
                        "output_dir": self.output_dir_var.get(),
                    },
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
        if self._worker is not None and self._worker.is_alive():
            self.cancel()
            self.status_var.set("Render is still stopping; close again when idle.")
            return
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
        self.session_dir_var = tk.StringVar()
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
        self.blend_var = tk.StringVar(value="feather")
        self.memory_var = tk.StringVar(value="768")
        self.allow_incomplete_var = tk.BooleanVar(value=False)
        self.coverage_var = tk.BooleanVar(value=False)
        self.status_var = tk.StringVar(value="Choose a capture JSON and screenshots directory.")

    def _build_widgets(self) -> None:
        paths = ttk.LabelFrame(self.root, text="Capture")
        paths.pack(fill="x", padx=12, pady=8)
        self._path_row(paths, 0, "Capture JSON", self.session_var, self._pick_session)
        self._path_row(paths, 1, "Screenshots", self.image_dir_var, self._pick_image_dir)
        self._path_row(paths, 2, "Output directory", self.output_dir_var, self._pick_output_dir)
        self._path_row(paths, 3, "Output filename", self.output_name_var, None)

        options = ttk.LabelFrame(self.root, text="Render options")
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
        self.advanced_button = ttk.Button(
            options, text="Advanced options ▸", command=self._toggle_advanced
        )
        self.advanced_button.grid(row=3, column=0, columnspan=2, sticky="w", padx=6, pady=4)
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
            "Memory budget (MiB)",
            ttk.Spinbox(
                self.advanced_frame, textvariable=self.memory_var, from_=1, to=8096, width=14
            ),
        )
        ttk.Label(
            self.advanced_frame, text="Larger budgets can render faster if RAM is available."
        ).grid(row=4, column=1, sticky="w", padx=6)
        ttk.Checkbutton(
            self.advanced_frame, text="Allow incomplete session", variable=self.allow_incomplete_var
        ).grid(row=5, column=1, sticky="w", padx=6, pady=3)
        ttk.Checkbutton(
            self.advanced_frame, text="Write coverage diagnostic PNG", variable=self.coverage_var
        ).grid(row=6, column=1, sticky="w", padx=6, pady=3)

        actions = ttk.Frame(self.root)
        actions.pack(fill="x", padx=12, pady=8)
        self.render_button = ttk.Button(
            actions, text="Render", command=self.render, state="disabled"
        )
        self.render_button.pack(side="left", padx=8)
        self.cancel_button = ttk.Button(
            actions, text="Cancel", command=self.cancel, state="disabled"
        )
        self.cancel_button.pack(side="left")

        self.progress = ttk.Progressbar(self.root, mode="determinate", maximum=100)
        self.progress.pack(fill="x", padx=12, pady=4)
        ttk.Label(self.root, textvariable=self.status_var, wraplength=640).pack(
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

    def _pick_session(self) -> None:
        chosen = filedialog.askopenfilename(
            title="Select capture metadata",
            initialdir=self.session_dir_var.get() or None,
            filetypes=(("JSON files", "*.json"), ("All files", "*.*")),
        )
        if chosen:
            self._set_path(self.session_var, chosen)
            session_path = Path(chosen).resolve()
            self._set_path(self.session_dir_var, session_path.parent)
            try:
                loaded = load_session(session_path, image_directory=session_path.parent)
                self._set_default_output_name(loaded.session_id)
                inferred = self._inferred_image_directory(loaded, session_path.parent)
                if inferred is not None:
                    self._set_path(self.image_dir_var, inferred)
            except (OSError, ValueError):
                pass

    def _pick_image_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select screenshots directory",
            initialdir=self.image_dir_var.get() or self.session_dir_var.get() or None,
        )
        if chosen:
            self._set_path(self.image_dir_var, chosen)

    def _pick_output_dir(self) -> None:
        chosen = filedialog.askdirectory(
            title="Select output directory",
            initialdir=self.output_dir_var.get() or self.session_dir_var.get() or None,
        )
        if chosen:
            self._set_path(self.output_dir_var, chosen)

    def _paths(self) -> tuple[Path, Path, Path]:
        session = Path(self.session_var.get()).expanduser().resolve()
        output_dir = Path(self.output_dir_var.get()).expanduser().resolve()
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
        output_dir.mkdir(parents=True, exist_ok=True)
        return session, image_dir, output_dir

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

    def _update_jpeg_quality_label(self) -> None:
        self.jpeg_quality_label_var.set(f"{self.jpeg_quality_var.get()}%")

    def _toggle_advanced(self) -> None:
        self._advanced_visible = not self._advanced_visible
        if self._advanced_visible:
            self.advanced_frame.grid(row=4, column=0, columnspan=2, sticky="ew", padx=6)
            self.advanced_button.configure(text="Advanced options ▾")
        else:
            self.advanced_frame.grid_remove()
            self.advanced_button.configure(text="Advanced options ▸")

    def _output_name_changed(self, *_args: object) -> None:
        self._output_name_dirty = True

    def _format_changed(self, *_args: object) -> None:
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

    def _set_default_output_name(self, session_id: str) -> None:
        if self._output_name_dirty:
            return
        self.output_name_var.set(f"panorama-{session_id}{self._suffix_for_format()}")
        self._output_name_dirty = False

    def _default_output_name(self, session_id: str, suffix: str) -> str:
        current = self.output_name_var.get().strip()
        if current and current != "panorama.png":
            return current
        return f"panorama-{session_id}{suffix}"

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

    def _input_changed(self, *_args: object) -> None:
        self._validated = False
        self.render_button.configure(state="disabled")
        if self._validation_after is not None:
            self.root.after_cancel(self._validation_after)
        if self.session_var.get().strip() and self.image_dir_var.get().strip():
            self._validation_after = self.root.after(300, self._start_validation)
        else:
            self.status_var.set("Choose a capture JSON and screenshots directory.")

    def _start_validation(self) -> None:
        self._validation_after = None
        if self._worker is None or not self._worker.is_alive():
            self._start_worker("validate")

    def _start_worker(self, operation: str) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        self._set_busy(True)
        self._cancel_event = threading.Event()
        self._worker = threading.Thread(target=self._worker_main, args=(operation,), daemon=True)
        self._worker.start()

    def render(self) -> None:
        if not self._validated:
            return
        output_path = self._output_path_preview()
        coverage_path = (
            output_path.with_name(f"{output_path.stem}-coverage.png")
            if output_path is not None and self.coverage_var.get()
            else None
        )
        existing = [
            path
            for path in (output_path, coverage_path)
            if path is not None and path.exists()
        ]
        if existing:
            if not messagebox.askyesno(
                "Overwrite existing file?",
                "The following output files already exist:\n"
                + "\n".join(path.name for path in existing)
                + "\nReplace them?",
            ):
                return
        self._start_worker("render")

    def cancel(self) -> None:
        if self._cancel_event is not None:
            self._cancel_event.set()
            self.status_var.set("Cancellation requested…")

    def _worker_main(self, operation: str) -> None:
        try:
            LOGGER.info("%s started", operation)
            session_path, image_dir, output_dir = self._paths()
            session = load_session(session_path, image_directory=image_dir)
            allow_incomplete = self.allow_incomplete_var.get()
            validate_images(session, image_dir, allow_incomplete)
            if operation == "validate":
                self._events.put(("validated", f"Valid session: {session.session_id}"))
                return
            session = renderable_session(session, image_dir, allow_incomplete)
            scale = Fraction(self.resolution_percent_var.get(), 100)
            if scale <= 0 or scale > 1:
                raise ValueError("resolution must be between 1/1 and a positive fraction")
            width = int(self.width_var.get()) if self.width_var.get().strip() else None
            if width is not None and width < 1:
                raise ValueError("explicit width must be positive")
            if width is not None and scale != 1:
                raise ValueError("explicit width and resolution fraction cannot be combined")
            memory = int(self.memory_var.get()) * 1024 * 1024
            if memory < 1 * 1024 * 1024 or memory > 8096 * 1024 * 1024:
                raise ValueError("memory budget must be between 1 and 8096 MiB")
            render_width = width
            if render_width is None and scale != 1:
                full = estimate_render_resources(session, image_dir, None, memory)
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
            resources = estimate_render_resources(session, image_dir, render_width, memory)
            self._events.put(
                ("status", f"Rendering {resources.output_width}×{resources.output_height}…")
            )
            exposure_report = render_session(
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
            )
            LOGGER.info("render completed: %s", output_path)
            self._events.put(
                (
                    "success",
                    f"Wrote {output_path} (exposure edges: {exposure_report.edge_count})",
                )
            )
        except RenderCancelledError:
            LOGGER.info("%s cancelled", operation)
            self._events.put(("cancelled", "Render cancelled; partial files were removed."))
        except Exception as error:
            LOGGER.exception("%s failed", operation)
            self._events.put(
                ("error", f"{error}\n\nDetails were written to {self._log_path()}")
            )
        finally:
            self._events.put(("idle", ""))

    def _progress(self, completed: int, total: int, phase: str) -> None:
        self._events.put(("progress", (completed, total, phase)))

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
                elif kind == "validated":
                    self._validated = True
                    self.render_button.configure(state="normal")
                    self.status_var.set(payload)
                elif kind == "success":
                    self.status_var.set(payload)
                    messagebox.showinfo("Panorama stitcher", payload)
                elif kind == "error":
                    self.status_var.set(f"Error: {payload}")
                    messagebox.showerror("Panorama stitcher", payload)
                elif kind == "cancelled":
                    self.status_var.set(payload)
                elif kind == "idle":
                    self._set_busy(False)
        except queue.Empty:
            pass
        self.root.after(100, self._drain_events)

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
        self.render_button.configure(state=state if self._validated else "disabled")
        self.cancel_button.configure(state="normal" if busy else "disabled")


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
        )
    except OSError:
        logging.basicConfig(level=logging.INFO)
    LOGGER.info("Panorama Stitcher started")


def main() -> None:
    """Launch the desktop stitcher frontend."""

    _configure_logging()
    root = tk.Tk()
    _configure_scaling(root)
    StitcherApp(root)
    root.mainloop()
