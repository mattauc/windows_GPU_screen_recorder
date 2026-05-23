# GPU Screen Recorder for Windows — Project Plan

> A Windows-native ShadowPlay clone. Pure GPU pipeline. Minimal CPU overhead.
> Inspired by the Linux project [dec05eba/gpu_screen_recorder](https://github.com/flathub/com.dec05eba.gpu_screen_recorder).

---

## 1. North Star

ShadowPlay beats OBS because **frames never leave GPU memory**:

```
[Desktop/Game frame on GPU] → [NVENC on GPU] → [encoded H.264/HEVC] → [CPU mux ~5 MB/s into MP4]
```

OBS's path (what we are NOT doing):

```
[GPU frame] → [download to CPU] → [color convert] → [upload to GPU] → [NVENC]
            → [download bitstream] → [filters/scenes] → [mux]
```

Every GPU↔CPU copy is a PCIe round-trip + stall. We avoid all of them.

**The entire pipeline must operate on `ID3D11Texture2D` handles, never `uint8_t*` pixel buffers.**

Every design decision serves this rule.

---

## 2. Locked Decisions

| Decision | Choice | Rationale |
|---|---|---|
| **GPU support v1** | NVIDIA only | Matches actual ShadowPlay scope. Add AMF/QSV in Phase 4 once architecture is proven. |
| **In-game overlay** | Core to v1 (two-tier strategy — see §6) | Required for ShadowPlay parity |
| **UI** | Headless daemon first, WinUI 3 in Phase 3 | Dogfood with CLI before investing in UI |
| **Feature scope** | Core ShadowPlay only: record, replay, hotkeys, overlay | Skip Highlights (per-game integrations are a tarpit), Broadcast, Photo Mode |
| **Dev environment** | Write code on macOS, build & test on Windows via CI / remote machine | See §10 |

---

## 3. Tech Stack (locked)

```
Language:     C++20, MSVC v143 (Visual Studio 2022)
Build:        CMake 3.28+ with vcpkg manifest mode
Graphics:     DirectX 11
Capture:      Windows Graphics Capture (WGC) — primary, Win10 1903+
              Desktop Duplication API — fallback
Encoder:      NVENC via NVIDIA Video Codec SDK 12.2+   ← v1 only
              IEncoder interface kept clean for AMF/QSV later
Audio:        WASAPI loopback (system) + WASAPI capture (mic)
              → Media Foundation AAC encoder
Muxer:        FFmpeg libavformat (fragmented MP4 default)
Overlay v1:   Win32 layered window + Direct2D + DirectWrite
Overlay v1.5: MinHook on IDXGISwapChain::Present + Dear ImGui
Hotkeys:      RegisterHotKey + Raw Input fallback (exclusive FS)
IPC:          Named pipe, length-prefixed MessagePack
UI (Phase 3): WinUI 3 (C# host, C++/WinRT for IPC)
Logging:      spdlog + ETW provider (debug with WPA)
Crash:        Local minidumps via Crashpad (no telemetry service in v1)
Installer:    WiX Toolset v4 → signed MSI (per-user, no admin)
Config:       %APPDATA%\GpuScreenRecorder\config.json
```

**Explicitly rejected:**
- **Qt** — too heavy for the daemon; WinUI more native for the UI app.
- **Rust** — NVENC/AMF docs and samples are all C++; bindings add friction. Revisit later.
- **Electron** — defeats the entire "low CPU" premise. Absolutely not.
- **OBS plugin route** — pulls in OBS's whole architecture. Wrong direction.

---

## 4. Architecture

Two processes communicating over a named pipe:

```
┌──────────────────────────────────────┐      ┌─────────────────────────┐
│  gpur-daemon.exe  (background)       │ ◄──► │ gpur-ui.exe  (WinUI 3)  │
│  - Capture                           │      │ - Settings              │
│  - Encode                            │      │ - Recording browser     │
│  - Mux                               │      │ - Hotkey config         │
│  - Hotkey listener                   │      │ - Per-game profiles     │
│  - Replay ring buffer                │      └─────────────────────────┘
│  - In-game overlay                   │
└──────────────────────────────────────┘
```

The daemon has no UI thread. The UI app can be killed without affecting recording.
This mirrors how NVIDIA splits ShadowPlay (Container Service) from GeForce Experience (UI shell).

### Daemon internals — the zero-copy pipeline

```
                    ┌──────────────────────────────────────────┐
                    │            Capture Thread                │
  WGC frame pool  ──► acquires ID3D11Texture2D (BGRA)          │
                    └────────────┬─────────────────────────────┘
                                 │ shared D3D11 device
                    ┌────────────▼─────────────────────────────┐
                    │       Color-Space Convert (GPU)          │
                    │  BGRA → NV12 via compute shader          │
                    │  Single dispatch, stays on GPU           │
                    └────────────┬─────────────────────────────┘
                                 │ ID3D11Texture2D (NV12)
                    ┌────────────▼─────────────────────────────┐
                    │           Encoder Thread                 │
                    │  NVENC consumes texture via              │
                    │  nvEncRegisterResource → encoded NALUs   │
                    └────────────┬─────────────────────────────┘
                                 │ encoded packets
                    ┌────────────▼─────────────────────────────┐
                    │   Ring Buffer (replay) OR Muxer (record) │
                    └──────────────────────────────────────────┘

                    ┌──────────────────────────────────────────┐
                    │       Audio Thread (parallel)            │
                    │  WASAPI loopback + mic → mix → AAC → mux │
                    └──────────────────────────────────────────┘
```

**One shared `ID3D11Device` across capture / convert / encode.** NVENC's
`nvEncRegisterResource` takes the same texture pointer — that's the zero-copy contract.

### Instant Replay design

- Ring buffer holds **encoded** packets in CPU RAM (encoded is ~1000× smaller than raw).
- 5 min @ 50 Mbps ≈ 1.8 GB — acceptable.
- Force a keyframe every 2 s (configurable). Same as ShadowPlay.
- Trade-off: slightly larger files vs. clean replay trims at exact start point.
- On "save replay" we find the last keyframe ≤ start-time and trim cleanly. **No re-encoding.**
- Audio ring buffer aligned to video PTS so sync is preserved on trim.

---

## 5. Defaults

| Setting | Default | Why |
|---|---|---|
| Container | Fragmented MP4 (`+frag_keyframe+empty_moov`) | Playable even if process crashes |
| Video codec | H.264 (HEVC opt-in) | Universal; HEVC breaks Discord/Twitter |
| Bitrate | 50 Mbps @ 1080p60, scaled linearly with resolution | Matches ShadowPlay "High" preset |
| Keyframe interval | 2 s | Clean replay trims |
| Rate control | CBR | Predictable file size |
| Audio | 192 kbps AAC stereo, 48 kHz | Discord/upload-friendly |
| Replay length | 5 minutes | ShadowPlay default |
| Output dir | `%USERPROFILE%\Videos\GpuScreenRecorder\` | |

---

## 6. Overlay Strategy

The single highest-risk piece. Split into two tiers.

### Tier 1 — Layered Window (ships in v1, Phase 2)

- Topmost transparent layered window: `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`
- Renders toast notifications via Direct2D + DirectWrite:
  - "Recording Started" / "Recording Stopped"
  - "Replay Saved: filename.mp4"
  - On-air red dot in corner while recording
- **Works for every game using borderless-fullscreen or windowed** — Valorant, OW2, Fortnite, Apex, all modern Steam titles.
- **No DLL injection. No anti-cheat risk.**

### Tier 2 — DXGI Hook (v1.5, Phase 4.5)

- MinHook on `IDXGISwapChain::Present` to draw inside the game's swap chain.
- Required only for true exclusive-fullscreen games (CS2, older titles).
- **Opt-in per game** in config. Documented anti-cheat risks upfront (EAC, BattlEye, VAC).
- ImGui-based, drawn with the game's own D3D device.

---

## 7. Module Layout

```
gpur/
├── core/
│   ├── d3d_context.{h,cpp}          // shared D3D11 device + queue
│   ├── capture/
│   │   ├── icapture.h
│   │   ├── wgc_capture.cpp          // Windows.Graphics.Capture (primary)
│   │   ├── dda_capture.cpp          // Desktop Duplication (fallback)
│   │   └── window_tracker.cpp       // follow a window across monitors
│   ├── convert/
│   │   └── bgra_to_nv12.hlsl        // compute shader
│   ├── encode/
│   │   ├── iencoder.h
│   │   ├── nvenc_encoder.cpp        // v1
│   │   ├── amf_encoder.cpp          // Phase 4
│   │   └── qsv_encoder.cpp          // Phase 4
│   ├── audio/
│   │   ├── wasapi_loopback.cpp
│   │   ├── wasapi_mic.cpp
│   │   └── mixer.cpp                // sample-rate convert + mix
│   ├── mux/
│   │   ├── ffmpeg_muxer.cpp         // libavformat wrapper
│   │   └── replay_buffer.cpp        // in-RAM encoded-packet ring
│   └── pipeline.cpp                  // wires it all together
├── daemon/
│   ├── main.cpp
│   ├── ipc_server.cpp                // named pipe protocol
│   ├── hotkey.cpp                    // RegisterHotKey + Raw Input
│   └── overlay/
│       ├── layered_window.cpp        // Tier 1 (v1)
│       ├── d3d11_hook.cpp            // Tier 2 (v1.5)
│       └── imgui_overlay.cpp
├── ui/                               // WinUI 3 project, Phase 3
│   ├── App.xaml
│   ├── Pages/Settings.xaml
│   ├── Pages/Recordings.xaml
│   └── Ipc/PipeClient.cs
├── shared/
│   └── ipc_protocol.h                // shared by daemon + UI
├── third_party/
│   ├── nvenc_sdk/                    // vendored
│   ├── amf/                          // vendored (Phase 4)
│   └── onevpl/                       // vendored (Phase 4)
├── installer/
│   └── gpur.wxs                      // WiX MSI (Phase 5)
├── .github/workflows/
│   └── build-windows.yml             // CI on windows-latest runners
├── CMakeLists.txt
├── vcpkg.json                        // manifest mode
├── PLAN.md                           // this file
└── README.md
```

---

## 8. Roadmap

| Phase | Scope | Est. | Ship criterion |
|---|---|---|---|
| **0 — Tracer bullet** | WGC → NVENC → raw `.h264` file. No audio, no UI, no mux. | 1 wk | Task Manager shows < 5% CPU during 1080p60 recording. If this fails, the whole premise is broken. |
| **1 — MVP recorder** | BGRA→NV12 shader, FFmpeg mux, MP4 output, WASAPI loopback+mic, AAC, hotkeys, CLI config | 2 wks | Record a 10-minute clip of a real game with audio. File plays in VLC, Windows Media Player, and uploads cleanly to Discord. |
| **2 — Replay + Overlay v1** | Ring buffer, save-last-N hotkey, Direct2D layered-window toasts ("Recording", "Replay Saved") | 2 wks | Replay save works mid-game with on-screen confirmation. Tested in Valorant/OW2/Apex. |
| **3 — WinUI 3 app** | Settings, recordings browser, hotkey config, daemon ↔ UI named-pipe IPC | 2 wks | Non-technical user can configure and record without touching CLI/config files. |
| **4 — AMD + Intel** | Implement AMF and oneVPL backends behind `IEncoder`, auto-detect GPU | 1.5 wks | Recording works on AMD RDNA2+ and Intel Arc. |
| **4.5 — Overlay v1.5** | Optional DXGI Present hook for exclusive-fullscreen games | 1 wk | Toast renders inside CS2 / older exclusive-FS titles. Opt-in per game. |
| **5 — Polish** | HDR support, signed MSI installer, crash reporting, per-game profiles | 2 wks | Installable on a fresh Windows machine via signed MSI in one click. |

**Total: ~12 weeks of focused work to a real 1.0.**

---

## 9. Risk Register

| Risk | Likelihood | Mitigation |
|---|---|---|
| WGC has higher overhead than NvFBC on NVIDIA | Medium | Add NvFBC backend in Phase 1.5 if benchmarks vs ShadowPlay show a gap |
| Encoder texture interop fails on some driver versions | Medium | Maintain known-good driver matrix; fall back to staging copy with loud warning |
| Tier 2 overlay hooking breaks anti-cheat | High | Tier 2 is opt-in per game; document EAC/BattlEye/VAC conflicts upfront |
| Exclusive fullscreen blocks `RegisterHotKey` | High | Raw Input secondary path (works in exclusive FS) |
| Audio drift over long recordings | Medium | Timestamp everything off `QueryPerformanceCounter`; resample audio to lock to video PTS |
| MP4 corruption on crash | High | Fragmented MP4 default — playable even if process dies mid-record |
| HDR tone-mapping looks wrong | Medium | Punt to Phase 5; SDR-first |
| Dev loop is slow (Mac → Windows CI) | Medium | See §10 — recommend remote Windows VM for graphics iteration |

---

## 10. Development Environment

**Decision: Write code on macOS, build & test on Windows.**

This is workable but graphics code has a fast iteration loop requirement. Strategy:

### Code authoring (macOS)
- VS Code or CLion with CMake + clangd.
- Cross-platform headers (`<cstdint>`, std lib) work fine.
- Windows-only headers won't resolve — that's expected; CI catches it.

### Build & test (Windows)
- **GitHub Actions `windows-latest` runners** for every push.
  - Free for public repos, generous quota for private.
  - Caches vcpkg installed packages.
  - Builds + runs unit tests on every commit.
- **Remote Windows machine** for graphics iteration (PCIe + GPU required, not available in standard CI).
  - **Recommended:** Personal Windows PC with NVIDIA GPU, accessed via RDP/Parallels/Tailscale SSH.
  - **Alternative:** Cloud GPU instance (Vultr Bare Metal, AWS `g4dn.xlarge`, ~$0.50-1.50/hr). Cost-effective for short focused sessions, painful as a permanent workflow.
- **No emulation path.** Wine/CrossOver won't run DXGI capture or NVENC. Windows is mandatory.

### Recommended physical setup
1. Mac for editor + git + planning.
2. Windows PC (or dual-boot) with discrete NVIDIA GPU for build/test.
3. Sync via git push/pull — no shared drives, no SMB shenanigans.
4. CI as the source of truth for "does it build clean."

---

## 11. Repo Setup Checklist (Day 1)

- [ ] `git init` on this directory
- [ ] Create GitHub repo (private to start)
- [ ] Add `.gitignore` (CMake build dirs, VS junk, `third_party/*/build/`)
- [ ] Add `LICENSE` — GPL-3.0 to match dec05eba's project, or MIT for permissive
- [ ] Add `README.md` with quickstart pointing at `PLAN.md`
- [ ] Add `CMakeLists.txt` skeleton + `vcpkg.json` manifest
- [ ] Add `.github/workflows/build-windows.yml`
- [ ] Vendor NVIDIA Video Codec SDK 12.2+ under `third_party/nvenc_sdk/`
  - Requires NVIDIA developer account, manual download (no package manager)
- [ ] Stub `core/capture/icapture.h` and `core/encode/iencoder.h` interfaces
- [ ] First commit on `main`, push to GitHub, verify CI green on empty skeleton

---

## 12. Open Questions for Later

Things we deferred but will need answers to before the relevant phase:

- **License:** GPL-3.0 (copyleft, matches reference project) vs MIT (permissive, friendlier to redistribution). Decide before first public push.
- **Telemetry:** None in v1. Revisit if we ever want crash report aggregation.
- **Auto-update:** None in v1 (manual MSI install). Squirrel/WinSparkle later if it becomes a real product.
- **Signing certificate:** Required for unsigned-MSI SmartScreen warning. Code-signing cert is ~$200/yr. Punt to Phase 5.
- **Streaming (RTMP):** Explicitly out of scope for now. If added, separate phase.

---

## 13. Definition of Done for v1.0

- [ ] Records 1080p60 H.264 at 50 Mbps with < 5% CPU on a mid-range NVIDIA GPU
- [ ] Saves last 5 min of gameplay via hotkey, in-game, with on-screen confirmation
- [ ] Works in borderless-fullscreen games without DLL injection
- [ ] Output MP4 plays everywhere (VLC, WMP, browsers, Discord upload)
- [ ] No audio drift over a 1-hour recording
- [ ] Settings configurable via WinUI app
- [ ] Installable via signed MSI on a fresh Windows 10/11 machine
- [ ] Daemon survives UI app crash; UI reconnects automatically
- [ ] Documented limitations: exclusive-fullscreen overlay opt-in, NVIDIA-only

Once these check off, v1.0 ships. Everything else is v1.1+.
