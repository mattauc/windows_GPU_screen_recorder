# third_party/

Vendored SDKs and library headers. None of this is committed to git directly — see `.gitignore`. You fetch them on your machine before building.

## NVIDIA Video Codec SDK (required for NVENC backend)

1. Sign in to https://developer.nvidia.com (free NVIDIA developer account).
2. Download **Video Codec SDK 12.2 or newer** from https://developer.nvidia.com/video-codec-sdk.
3. Extract such that:

   ```
   third_party/nvenc_sdk/
       Interface/
           nvEncodeAPI.h
           nvcuvid.h
           ...
       Lib/
       Samples/
       doc/
   ```

   Only the `Interface/` directory is needed at build time. CMake's `GPUR_BUILD_NVENC` option auto-detects the SDK by checking for `Interface/nvEncodeAPI.h`. If the directory is missing, NVENC is built as a stub backend that returns `Error::Code::NotImplemented` at runtime.

4. Verify:
   ```powershell
   cmake --preset windows-msvc
   # Look for: "NVENC backend  : ON"
   ```

## AMD AMF (Phase 4)

When Phase 4 (AMD support) starts:

1. Clone https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git into `third_party/amf/`.
2. We only need the `amf/public/include/` headers — runtime resolves via `amfrt64.dll` shipped with AMD drivers.

## Intel oneVPL (Phase 4)

When Phase 4 (Intel support) starts:

1. `vcpkg install onevpl` — header-only consumer, no manual vendoring required.

## Why not vcpkg for NVENC?

NVENC SDK is gated behind NVIDIA's developer agreement and not redistributable via package managers. Manual download is required. The headers themselves are tiny (~200 KB) — once downloaded the build is fast.

## CI

GitHub Actions runners don't have the NVENC SDK (or NVIDIA GPUs). The CI build uses `windows-msvc-ci` preset which sets `GPUR_BUILD_NVENC=OFF`, so the NVENC backend compiles to its stub. Real NVENC paths are only tested on your local Windows machine.
