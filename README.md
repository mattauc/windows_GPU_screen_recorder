# GPU Screen Recorder for Windows

A Windows-native ShadowPlay clone. Pure GPU pipeline. Minimal CPU overhead.

Captures the desktop / a game window with **Windows Graphics Capture**, color-converts on the GPU with a compute shader, and encodes with **NVENC** — all without round-tripping through CPU memory. The CPU only writes the encoded bitstream to disk.

> **Status:** Phase 0 (tracer bullet) — proving the zero-copy capture → NVENC path works. See [`PLAN.md`](./PLAN.md) for the full roadmap.

---

## Quickstart (Windows)

**Prerequisites:**
- Windows 10 1903+ or Windows 11
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.28+
- [vcpkg](https://github.com/microsoft/vcpkg) bootstrapped with the `VCPKG_ROOT` env var set
- NVIDIA GPU + recent driver
- [NVIDIA Video Codec SDK 12.2+](https://developer.nvidia.com/video-codec-sdk) — extracted to `third_party/nvenc_sdk/`. See [`third_party/README.md`](./third_party/README.md).

**Build:**

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
```

**Run (Phase 0 tracer bullet):**

```powershell
.\out\build\windows-msvc\daemon\Debug\gpur-daemon.exe `
    --output test.h264 `
    --duration 10 `
    --fps 60 `
    --bitrate 50000000
```

Open `test.h264` in VLC. Confirm Task Manager shows < 5% CPU during recording.

---

## Repository layout

See [`PLAN.md` §7](./PLAN.md#7-module-layout) for the canonical module breakdown.

```
core/         capture / encode / convert / audio / mux  (the engine)
daemon/       background process — recorder + IPC + overlay
ui/           WinUI 3 settings app (Phase 3)
shared/       headers shared by daemon + UI (IPC protocol, logging)
third_party/  vendored SDKs (NVIDIA Video Codec SDK, AMF, oneVPL)
installer/    WiX MSI project (Phase 5)
PLAN.md       full project plan + roadmap
BUILDING.md   detailed build instructions
```

---

## Development environment

Source is authored on macOS, built and tested on Windows. See [`PLAN.md` §10](./PLAN.md#10-development-environment).

CI runs on `windows-latest` GitHub Actions runners for every push, with NVENC stubbed out (the SDK requires a developer account so we can't ship it in CI). Local Windows builds get the full NVENC backend.

---

## License

MIT (see [`LICENSE`](./LICENSE)). Subject to change before public release.
