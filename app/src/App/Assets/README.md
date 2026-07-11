# App icon assets

The `.ico` files here are the real URnetwork brand icons, generated from the
macOS asset catalog (`apple/app/network/Assets.xcassets`) by
`tools/make-icons.py` (Pillow). They are committed (small, stable, needed at
build time). Regenerate when the brand art changes:

```
python3 tools/make-icons.py
```

`App.rc` maps each to a resource id in `resource.h`.

| File | Source (macOS) | Meaning |
|---|---|---|
| `app.ico` | `AppIcon.appiconset` 1024px | app icon (16–256) |
| `tray_light_*` | `MenuBarLight*` (dark ink) | tray on a **light** taskbar |
| `tray_dark_*` | `MenuBarDark*` (white ink) | tray on a **dark** taskbar |

The four `noprovide/provide × noconnect/connect` states map 1:1 to the macOS
`MenuBar{Provide,NoProvide}{Connect,NoConnect}` art. macOS "Light"/"Dark" name
the *appearance*, so Light = dark ink (for light taskbars) and Dark = white ink
(for dark taskbars); `TrayIcon.cpp` picks by `SystemUsesLightTheme`. Each `.ico`
carries 16/20/24/32/48 (tray) or 16–256 (app) sizes.
