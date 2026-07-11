#!/usr/bin/env python3
# Generates the Windows tray + app .ico files from the macOS asset catalog art
# (apple/app/network/Assets.xcassets). The macOS menu-bar images already carry
# the four connect x provide states in light/dark variants; this maps them to
# the Windows resource ids (see src/App/resource.h) and app.rc. Run whenever the
# brand art changes; the .ico outputs are committed.
#
# Requires Pillow. Usage: python3 tools/make-icons.py
#
# SPDX-License-Identifier: MPL-2.0
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
APP_ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "..", "apple", "app",
                                           "network", "Assets.xcassets"))
OUT = os.path.normpath(os.path.join(HERE, "..", "src", "App", "Assets"))

# macOS naming: "Light" = art for light appearance (dark ink) -> Windows light
# taskbar; "Dark" = art for dark appearance (white ink) -> Windows dark taskbar.
STATES = ["NoProvideNoConnect", "NoProvideConnect", "ProvideNoConnect", "ProvideConnect"]
TRAY_SIZES = [16, 20, 24, 32, 48]
APP_SIZES = [16, 24, 32, 48, 64, 128, 256]


def menubar_src(mode, state):
    d = os.path.join(APP_ASSETS, "Icons", f"MenuBar{state}.imageset")
    return os.path.join(d, f"MenuBar{mode}{state}32.png")


def app_src():
    d = os.path.join(APP_ASSETS, "AppIcon.appiconset")
    for name in os.listdir(d):
        if "1024" in name and name.endswith(".png"):
            return os.path.join(d, name)
    # fallback: largest png
    pngs = [os.path.join(d, n) for n in os.listdir(d) if n.endswith(".png")]
    return max(pngs, key=lambda p: os.path.getsize(p))


def save_ico(src_png, out_name, sizes):
    img = Image.open(src_png).convert("RGBA")
    out_path = os.path.join(OUT, out_name)
    img.save(out_path, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"  {out_name}  <- {os.path.relpath(src_png, APP_ASSETS)}")


def main():
    os.makedirs(OUT, exist_ok=True)
    snake = {"NoProvideNoConnect": "noprovide_noconnect",
             "NoProvideConnect": "noprovide_connect",
             "ProvideNoConnect": "provide_noconnect",
             "ProvideConnect": "provide_connect"}
    print("tray icons:")
    for state in STATES:
        # macOS Light -> windows light taskbar, macOS Dark -> windows dark taskbar
        save_ico(menubar_src("Light", state), f"tray_light_{snake[state]}.ico", TRAY_SIZES)
        save_ico(menubar_src("Dark", state), f"tray_dark_{snake[state]}.ico", TRAY_SIZES)
    print("app icon:")
    save_ico(app_src(), "app.ico", APP_SIZES)
    print("done.")


if __name__ == "__main__":
    main()
