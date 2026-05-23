# ui/ — WinUI 3 settings app (Phase 3)

Empty placeholder. The WinUI app lives here once Phase 3 starts.

## Why a separate project?

WinUI 3 uses MSBuild + Windows App SDK, not CMake. We keep the recorder daemon
(C++/CMake) and the UI shell (C#/MSBuild) in separate build systems so neither
project drags the other's tooling around.

Communication: named-pipe IPC (see `shared/ipc_protocol.h`).

## Planned layout

```
ui/
├── GpurUi.sln
├── GpurUi/
│   ├── GpurUi.csproj
│   ├── App.xaml
│   ├── App.xaml.cs
│   ├── MainWindow.xaml
│   ├── Pages/
│   │   ├── Settings.xaml
│   │   ├── Recordings.xaml
│   │   ├── Hotkeys.xaml
│   │   └── GameProfiles.xaml
│   ├── Ipc/
│   │   └── PipeClient.cs
│   └── Models/
└── README.md
```

Don't add WinUI/csproj scaffolding until Phase 3 is ready to start — premature
scaffolding here would block the daemon's tighter iteration loop.
