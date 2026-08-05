# App assets

## Brand fonts (`Fonts/`)

The four URnetwork brand faces, copied byte-for-byte from the android repo
(`app/app/src/main/res/font/`), which is where they are version-controlled for
the product. **These are licensed commercial faces** (ABC Gravity is Dinamo,
PP Neue Montreal / PP NeueBit are Pangram Pangram): they ship inside the app and
must not be redistributed on their own.

XAML references a bundled font as `ms-appx:///<path>#<family>`, where the family
is the font's **own internal family name** — the OpenType `name` table, id 1 —
and *not* the file name. Getting it wrong fails silently: the text just renders
in the fallback face. These were read out of the files, not guessed:

| File | Family name (name id 1) | Faces in file | Android role (`ui/theme/Type.kt`) |
|---|---|---|---|
| `abcgravity_extended.otf` | `ABC Gravity Extended` | Regular | `headlineLarge`, `headlineSmall` |
| `abcgravity_extra_condensed.otf` | `ABC Gravity Extra Condensed` | Regular | `headlineMedium` |
| `pp_neue_montreal_regular.ttf` | `PP Neue Montreal` | Regular | `bodyLarge/Medium/Small` |
| `pp_neue_bit_bold.ttf` | `PP NeueBit` | **Bold only** | `TopBarTitleTextStyle` |

Two traps in that table. Both ABC Gravity files share the *typographic* family
`ABC Gravity` (name id 16) and differ only by subfamily, so the full id-1 name is
the unambiguous one to reference. And `PP NeueBit` has no space in `NeueBit`, and
ships only its Bold face — anything using it states `FontWeight="Bold"` so the
real face is selected rather than synthesised from it.

`App.xaml` defines the four `FontFamily` resources; `App.vcxproj` copies the
files to `$(OutDir)Assets\Fonts` so an unpackaged app can resolve `ms-appx:///`
against the exe's own folder.

### Coverage: these are Latin-only faces

Read out of each `cmap`: PP Neue Montreal 596 codepoints, PP NeueBit 592, both
ABC Gravity cuts 464. Latin, punctuation, arrows, the currency marks — and
nothing else. Two consequences, neither of them new (android ships the same four
files and inherits the same behaviour), but both worth knowing:

- **The app ships 28 locales.** Arabic, Hebrew, Hindi, Japanese, Korean, Thai,
  Cyrillic and both Chinese variants have no glyphs in these fonts, so those UIs
  render entirely through DirectWrite's font fallback. That is correct and
  legible — it is simply not the brand face. Nothing to fix; do not "fix" it by
  reverting the fonts.
- **U+03C4, the Bittensor tau, is in none of them.** `TauGlyph` in
  `MainWindow.xaml` therefore pins `Segoe UI` explicitly rather than inheriting
  the body face and letting fallback choose for us.

**If the text still renders in the fallback face**, the reference failed and said
nothing — that is the only failure mode fonts have here. Check, in order:

1. `Assets\Fonts\*` really are next to `URnetwork.exe`. An MSI install does not
   carry them yet (`installer/Package.wxs` installs no assets at all); running
   from the build output does.
2. The family name after `#`. Re-read it rather than trusting this table:
   `python tools/...` is not needed — any font inspector, or PowerShell's
   `[System.Drawing.Text.PrivateFontCollection]`, will report name id 1.
3. The URI form. `ms-appx:///` is what the rest of this app uses (see
   `ReferralFrog.png` in `MainWindow.xaml`) and is what WinUI documents, but an
   **unpackaged** app also accepts an exe-relative path — `/Assets/Fonts/
   pp_neue_montreal_regular.ttf#PP Neue Montreal` — which is worth trying if
   `ms-appx:///` resolves images but not fonts.

## App icon assets

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
