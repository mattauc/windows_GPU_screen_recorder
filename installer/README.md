# installer/ — WiX MSI (Phase 5)

Empty placeholder. The signed MSI installer lands here once v1 is feature-complete.

## Planned contents

```
installer/
├── gpur.wxs                  # WiX v4 source: components, features, UI
├── License.rtf               # MIT license shown in installer UI
├── Bundle.wxs                # Optional: prerequisites bundle
└── README.md
```

## Distribution plan

- Per-user MSI install to `%LOCALAPPDATA%\GpuScreenRecorder\` (no admin required).
- Start menu shortcut: "GPU Screen Recorder".
- Optional auto-start of `gpur-daemon.exe` on login.
- Code-signing certificate (~$200/yr from Sectigo/DigiCert) to avoid SmartScreen warnings.

Don't build until v1.0 features are stable — packaging churn is expensive.
