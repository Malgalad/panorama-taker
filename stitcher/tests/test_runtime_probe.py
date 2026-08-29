from __future__ import annotations

import importlib.util
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest

from pano_stitch.d3d12_adapter import D3D12AdapterUnavailableError


def _entrypoint_module() -> ModuleType:
    path = Path(__file__).resolve().parents[1] / "scripts" / "pano_stitch_gui.py"
    spec = importlib.util.spec_from_file_location("pano_stitch_gui_entrypoint", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_gpu_runtime_probe_reports_verified_hardware_adapter() -> None:
    module = _entrypoint_module()

    class Adapter:
        def probe(self) -> object:
            return SimpleNamespace(
                name=b"Test Adapter\0",
                vendor_id=0x10DE,
                device_id=0x1234,
                luid=0x5678,
            )

        def verify_runtime(self) -> None:
            pass

    result = module._verify_gpu_runtime(Adapter)

    assert "D3D12 runtime verified" in result
    assert "adapter=Test Adapter" in result
    assert "vendor=0x10de" in result


def test_gpu_runtime_probe_returns_unavailable_category(tmp_path: Path) -> None:
    module = _entrypoint_module()

    def unavailable() -> str:
        raise module._GpuRuntimeUnavailable("no compatible adapter")

    module._verify_gpu_runtime = unavailable
    result = tmp_path / "probe.txt"
    with pytest.raises(SystemExit, match="2"):
        module._run_gpu_runtime_probe(result)
    assert result.read_text() == "GPU runtime unavailable: no compatible adapter\n"


def test_gpu_runtime_probe_returns_failure_category_after_probe(tmp_path: Path) -> None:
    module = _entrypoint_module()

    class Adapter:
        def probe(self) -> object:
            return SimpleNamespace(name=b"Test\0", vendor_id=1, device_id=2, luid=3)

        def verify_runtime(self) -> None:
            raise D3D12AdapterUnavailableError("injected dispatch failure")

    verify = module._verify_gpu_runtime
    module._verify_gpu_runtime = lambda: verify(Adapter)
    result = tmp_path / "probe.txt"
    with pytest.raises(SystemExit, match="3"):
        module._run_gpu_runtime_probe(result)
    assert "injected dispatch failure" in result.read_text()
