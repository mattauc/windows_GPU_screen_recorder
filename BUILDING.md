# Building GPU Screen Recorder

## Supported platforms

Windows 10 1903+ or Windows 11. **The project does not build on macOS or Linux** — it depends on DirectX 11, WGC, NVENC, WASAPI, and the Win32 API.

If you author code on macOS, use GitHub Actions (`.github/workflows/build-windows.yml`) for compile-checks, and a Windows machine (physical or remote) for actual testing.

## Prerequisites

1. **Visual Studio 2022** with the **Desktop development with C++** workload installed.
   - Includes MSVC v143, Windows 10/11 SDK, CMake integration.
2. **CMake 3.28+** (`cmake --version`). Bundled with VS 2022 but newer is fine.
3. **vcpkg** in manifest mode:
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
   C:\dev\vcpkg\bootstrap-vcpkg.bat
   setx VCPKG_ROOT "C:\dev\vcpkg"
   # Restart your shell after setx
   ```
4. **NVIDIA Video Codec SDK 12.2 or newer** for the NVENC backend.
   - Download from https://developer.nvidia.com/video-codec-sdk (NVIDIA developer account required).
   - Extract such that `third_party/nvenc_sdk/Interface/nvEncodeAPI.h` exists.
   - See [`third_party/README.md`](./third_party/README.md).
5. **NVIDIA GPU + driver R535 or newer** to actually record. The build will succeed without one; runtime will not.

## Configuring + building

From a **Developer PowerShell for VS 2022** (so MSVC is on PATH):

```powershell
cd path\to\GPU_ScreenRecorder_Windows
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
```

Resulting binary: `out\build\windows-msvc\daemon\Debug\gpur-daemon.exe`.

Release build:

```powershell
cmake --build --preset windows-msvc-release
```

## CMake options

| Option | Default | Effect |
|---|---|---|
| `GPUR_BUILD_NVENC` | `ON` if NVENC SDK found, else `OFF` | Build the NVENC encoder backend. |
| `GPUR_BUILD_AMF`   | `OFF` (Phase 4) | Build the AMD AMF encoder backend. |
| `GPUR_BUILD_QSV`   | `OFF` (Phase 4) | Build the Intel Quick Sync backend. |
| `GPUR_BUILD_TESTS` | `ON` | Build unit tests (Catch2). |

Override at configure time:

```powershell
cmake --preset windows-msvc -DGPUR_BUILD_NVENC=OFF
```

## Running Phase 0

The Phase 0 tracer bullet is a CLI tool:

```powershell
.\out\build\windows-msvc\daemon\Debug\gpur-daemon.exe `
    --output recording.h264 `
    --duration 10 `
    --fps 60 `
    --bitrate 50000000
```

Flags:

| Flag | Default | Description |
|---|---|---|
| `--output PATH` | `recording.h264` | Output Annex-B H.264 stream (no container in Phase 0). |
| `--duration SEC` | `0` (until Ctrl+C) | Recording length in seconds. |
| `--fps N` | `60` | Capture / encode framerate. |
| `--bitrate BPS` | `50000000` | Encoder target bitrate (bits/sec). |
| `--monitor N` | `0` | Monitor index (0 = primary). |
| `--codec h264\|hevc` | `h264` | Encoder codec. |

**Verify the zero-copy goal:**
1. Start a game or animated content on your primary monitor.
2. Run the recorder for 10 seconds.
3. Watch Task Manager — `gpur-daemon.exe` should hover at **< 5% CPU**.
4. Play `recording.h264` in VLC. Smooth, correct colors, no judder.

If CPU is high, something is round-tripping to system memory. Profile with PIX or NSight.

## Troubleshooting

- **`fatal error: 'nvEncodeAPI.h' file not found`** — The NVENC SDK is missing or `third_party/nvenc_sdk/Interface/` doesn't exist. Either install the SDK or configure with `-DGPUR_BUILD_NVENC=OFF`.
- **`CMake Error: VCPKG_ROOT is not set`** — Set the env var per Prerequisites step 3, then restart your shell.
- **CMake configure on macOS / Linux** — Unsupported. The root `CMakeLists.txt` errors out explicitly.
- **NVENC fails to initialize at runtime** — Check `nvidia-smi` shows your GPU, and your driver is R535+. NVENC has session-count limits on GeForce cards (historically 3, now unlimited on R550+).
