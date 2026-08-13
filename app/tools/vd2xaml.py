#!/usr/bin/env python3
"""Convert an Android VectorDrawable into a WinUI 3 XAML fragment.

The brand icons live in the android repo as VectorDrawables. Their pathData is
already SVG path syntax, which WinUI's Path.Data accepts unchanged, so the
conversion is really just the wrapper: viewport -> Canvas size, fillColor ->
Fill, and aapt inline gradients -> LinearGradientBrush in Absolute mapping mode
(android states gradient endpoints in viewport units, not 0..1).

  python vd2xaml.py <in.xml> [--name Foo] [--size 18]

Writes the fragment to stdout. Wrap it in a Viewbox at the call site, or pass
--size to emit one.

SPDX-License-Identifier: MPL-2.0
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET

ANDROID = "http://schemas.android.com/apk/res/android"
AAPT = "http://schemas.android.com/aapt"


def a(el, name, default=None):
    return el.get(f"{{{ANDROID}}}{name}", default)


def dp(value, default=0.0):
    """'24dp' / '24' -> 24.0"""
    if value is None:
        return default
    return float(re.sub(r"[a-zA-Z]+$", "", value))


def argb(color):
    """Android #AARRGGBB or #RRGGBB -> XAML #AARRGGBB (same form, validated)."""
    if not color:
        return None
    c = color.strip()
    if not c.startswith("#"):
        return None
    if len(c) == 7 or len(c) == 9:
        return c
    if len(c) == 4:  # #RGB
        return "#" + "".join(ch * 2 for ch in c[1:])
    return c


def gradient_xaml(grad, indent):
    """<gradient> child of an aapt:attr name="android:fillColor"."""
    pad = " " * indent
    gtype = a(grad, "type", "linear")
    if gtype != "linear":
        # radial/sweep are not used by these icons; fail loudly rather than
        # silently emitting a flat fill that looks almost right
        raise SystemExit(f"unsupported gradient type: {gtype}")
    x1, y1 = dp(a(grad, "startX")), dp(a(grad, "startY"))
    x2, y2 = dp(a(grad, "endX")), dp(a(grad, "endY"))
    stops = []
    for item in grad.findall("item"):
        stops.append((dp(a(item, "offset")), argb(a(item, "color"))))
    if not stops:
        s, e = argb(a(grad, "startColor")), argb(a(grad, "endColor"))
        stops = [(0.0, s), (1.0, e)]
    out = [f'{pad}<LinearGradientBrush MappingMode="Absolute" '
           f'StartPoint="{x1},{y1}" EndPoint="{x2},{y2}">']
    for off, col in stops:
        out.append(f'{pad}  <GradientStop Offset="{off}" Color="{col}" />')
    out.append(f"{pad}</LinearGradientBrush>")
    return "\n".join(out)


def convert(path, name=None, size=None):
    tree = ET.parse(path)
    root = tree.getroot()
    vw = dp(a(root, "viewportWidth"), 24)
    vh = dp(a(root, "viewportHeight"), 24)

    body = []
    # NOTE: <path>/<gradient>/<item> are UNQUALIFIED elements -- only the
    # attributes live in the android namespace. Qualifying the element
    # names finds nothing and emits a silently empty icon.
    for p in root.findall("path"):
        data = a(p, "pathData")
        if not data:
            continue
        data = " ".join(data.split())
        fill = argb(a(p, "fillColor"))
        grad = None
        for attr in p.findall(f"{{{AAPT}}}attr"):
            if attr.get("name") == "android:fillColor":
                g = attr.find("gradient")
                if g is not None:
                    grad = g
        if grad is not None:
            body.append(f'      <Path Data="{data}">')
            body.append("        <Path.Fill>")
            body.append(gradient_xaml(grad, 10))
            body.append("        </Path.Fill>")
            body.append("      </Path>")
        else:
            f = f' Fill="{fill}"' if fill else ""
            body.append(f'      <Path{f} Data="{data}" />')

    inner = "\n".join(body)
    key = f' x:Key="{name}"' if name else ""
    if size:
        return (f'<Viewbox{key} Width="{size}" Height="{size}">\n'
                f'  <Canvas Width="{vw}" Height="{vh}">\n'
                f"{inner}\n"
                f"  </Canvas>\n"
                f"</Viewbox>")
    return (f'<Canvas{key} Width="{vw}" Height="{vh}">\n'
            f"{inner}\n"
            f"</Canvas>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--name")
    ap.add_argument("--size", type=float)
    args = ap.parse_args()
    print(convert(args.input, args.name, args.size))


if __name__ == "__main__":
    main()
