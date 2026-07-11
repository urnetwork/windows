# Vendored dependencies

These are fetched by `tools/fetch-deps.ps1`, not committed (except this README).
The `.gitignore` excludes the binary payloads.

## `urnetwork-sdk/{amd64,arm64}/`

The C ABI + C++ wrapper produced by `sdk/cgo`. Each arch dir holds:

- `URnetworkSdk.dll` — the SDK (embeds the Go runtime, wintun is separate)
- `URnetworkSdk.lib` — import library, generated from `urnetwork_sdk.def` via
  `lib /def:urnetwork_sdk.def /machine:{x64|arm64} /out:URnetworkSdk.lib`
- `urnetwork_sdk.h` — the raw C ABI
- `urnetwork_sdk.hpp` — the header-only C++17 wrapper (the API the app uses)

Built on the macOS build server: `make -C ../../sdk/cgo build_windows` produces
`sdk/cgo/build/URnetworkSdkWindows.zip` with `windows/{amd64,arm64}/` inside.
`fetch-deps.ps1` unzips it here and generates the import libs.

## `wintun/`

The upstream-signed Wintun DLLs (wintun.net). Pinned by SHA256 and Authenticode
signer thumbprint, exactly as Mullvad does (plan §2). Layout after fetch:

- `wintun.h`
- `bin/amd64/wintun.dll`, `bin/arm64/wintun.dll`

We redistribute these unmodified per the Wintun Prebuilt Binaries License; we do
not build or sign the driver ourselves.

## nlohmann-json, wil

Come from vcpkg (manifest mode, see `../vcpkg.json`) — not vendored here.
