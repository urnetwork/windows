#!/usr/bin/env python3
"""Deterministic contracts for warnings that previously survived release builds."""

from pathlib import Path
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
WIX_NS = "http://wixtoolset.org/schemas/v4/wxs"
UTIL_NS = "http://wixtoolset.org/schemas/v4/wxs/util"
MSBUILD_NS = "http://schemas.microsoft.com/developer/msbuild/2003"
FONT_NAMES = {
    "abcgravity_extended.otf",
    "abcgravity_extra_condensed.otf",
    "pp_neue_bit_bold.ttf",
    "pp_neue_montreal_regular.ttf",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def test_win32_macro_guards() -> None:
    source_root = ROOT / "app" / "src"
    for path in source_root.rglob("*"):
        if path.suffix.lower() not in {".cpp", ".h", ".hpp"}:
            continue
        lines = path.read_text(encoding="utf-8-sig").splitlines()
        for index, line in enumerate(lines):
            if line.strip() != "#define WIN32_LEAN_AND_MEAN":
                continue
            previous = index - 1
            while previous >= 0 and not lines[previous].strip():
                previous -= 1
            if previous < 0 or lines[previous].strip() != "#ifndef WIN32_LEAN_AND_MEAN":
                fail(f"{path.relative_to(ROOT)}:{index + 1}: unguarded WIN32_LEAN_AND_MEAN")

    props = ET.parse(ROOT / "app" / "Directory.Build.props").getroot()
    options = " ".join(
        node.text or "" for node in props.findall(f".//{{{MSBUILD_NS}}}AdditionalOptions")
    )
    for warning in ("/we4005", "/we4651"):
        if warning not in options:
            fail(f"Directory.Build.props does not keep {warning} fatal")


def test_generated_xaml_pch_contract() -> None:
    project = ET.parse(ROOT / "app" / "src" / "App" / "App.vcxproj").getroot()
    global_definitions = [
        node.text or ""
        for node in project.findall(
            f"./{{{MSBUILD_NS}}}ItemDefinitionGroup/"
            f"{{{MSBUILD_NS}}}ClCompile/{{{MSBUILD_NS}}}PreprocessorDefinitions"
        )
    ]
    if not any(
        "MICROSOFT_WINDOWSAPPSDK_SELFCONTAINED=1" in definitions
        for definitions in global_definitions
    ):
        fail(
            "generated XAML units cannot inherit the self-contained definition "
            "from the project-wide ClCompile defaults"
        )

    targets = project.findall(f".//{{{MSBUILD_NS}}}Target")
    target = next(
        (node for node in targets if node.attrib.get("Name") == "UrnCompileGeneratedXamlImpl"),
        None,
    )
    if target is None:
        fail("UrnCompileGeneratedXamlImpl target is missing")
    dynamic = next(
        (
            node
            for node in target.findall(f".//{{{MSBUILD_NS}}}ClCompile")
            if node.attrib.get("Include") == "@(_UrnGenXamlCpp)"
        ),
        None,
    )
    if dynamic is None:
        fail("generated XAML compile item is missing")
    definitions = dynamic.find(f"{{{MSBUILD_NS}}}PreprocessorDefinitions")
    if definitions is not None:
        fail(
            "generated XAML items override inherited ClCompile definitions; "
            "an unqualified metadata expansion fails with MSB4096"
        )


def test_installer_contract() -> None:
    package = ET.parse(ROOT / "app" / "installer" / "Package.wxs").getroot()
    native_delayed = [
        node
        for node in package.findall(f".//{{{WIX_NS}}}ServiceConfig")
        if "DelayedAutoStart" in node.attrib
    ]
    if native_delayed:
        fail("ordinary auto-start service redundantly authors delayed-auto-start metadata")
    if not package.findall(f".//{{{UTIL_NS}}}ServiceConfig"):
        fail("urnetworkd failure-action service configuration is missing")

    runtime = next(
        (
            node
            for node in package.findall(f".//{{{WIX_NS}}}ComponentGroup")
            if node.attrib.get("Id") == "RuntimeFiles"
        ),
        None,
    )
    if runtime is None:
        fail("RuntimeFiles component group is missing")
    harvested = runtime.find(f"{{{WIX_NS}}}Files")
    if harvested is None:
        fail("RuntimeFiles harvester is missing")
    excluded = {
        Path(node.attrib["Files"].replace("\\", "/")).name
        for node in harvested.findall(f"{{{WIX_NS}}}Exclude")
        if "Files" in node.attrib
    }
    if not FONT_NAMES.issubset(excluded):
        fail(f"private fonts are not excluded from generic harvesting: {sorted(FONT_NAMES - excluded)}")

    explicit = {}
    for node in runtime.findall(f"{{{WIX_NS}}}File"):
        source = node.attrib.get("Source", "").replace("\\", "/")
        name = Path(source).name
        if name in FONT_NAMES:
            explicit[name] = node
    if set(explicit) != FONT_NAMES:
        fail(f"private font metadata is incomplete: {sorted(FONT_NAMES - set(explicit))}")
    for name, node in explicit.items():
        if node.attrib.get("DefaultLanguage") != "0":
            fail(f"{name} is not authored as language-neutral")
        if node.attrib.get("Subdirectory", "").replace("\\", "/") != "Assets/Fonts":
            fail(f"{name} would not install under Assets/Fonts")
        if "TrueType" in node.attrib or "FontTitle" in node.attrib:
            fail(f"{name} would be registered globally instead of remaining app-local")

    wix_project = ET.parse(ROOT / "app" / "installer" / "Installer.wixproj").getroot()
    warning_policy = wix_project.find(".//TreatWarningsAsErrors")
    if warning_policy is None or (warning_policy.text or "").strip().lower() != "true":
        fail("WiX warnings are not fatal")


def test_neutral_plural_resources() -> None:
    strings = ROOT / "app" / "src" / "App" / "Strings"
    neutral_path = strings / "en" / "Resources.resw"
    neutral = {
        node.attrib["name"]
        for node in ET.parse(neutral_path).getroot().findall("data")
        if "name" in node.attrib
    }
    plural_suffixes = {"zero", "one", "two", "few", "many", "other"}
    qualified = set()
    for path in strings.glob("*/Resources.resw"):
        for node in ET.parse(path).getroot().findall("data"):
            name = node.attrib.get("name", "")
            if "." in name and name.rsplit(".", 1)[1] in plural_suffixes:
                qualified.add(name)
    missing = sorted(qualified - neutral)
    if missing:
        fail(f"neutral Resources.resw omits plural resource names: {missing[:8]}")


def main() -> int:
    tests = (
        test_win32_macro_guards,
        test_generated_xaml_pch_contract,
        test_installer_contract,
        test_neutral_plural_resources,
    )
    try:
        for case in tests:
            case()
    except (AssertionError, ET.ParseError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS: Windows build warning contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
