Import("env")

from pathlib import Path
import ast
import re
import ssl
import urllib.request

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
LIB_DIR = PROJECT_DIR / "lib" / "JC8012P4A1C_display"
SRC_DIR = LIB_DIR / "src"

# Pinned display driver revision used by Family Calendar 1.0.6.
DRIVER_COMMIT = "71cc9af19ef63c1761d1139069e3a1ece6682dfa"
DRIVER_BASE = (
    "https://raw.githubusercontent.com/chipguyhere/JC8012P4A1C_display/"
    + DRIVER_COMMIT + "/"
)

# Pinned ESPHome revision containing the tested JC8012P4A1-V2 profile.
ESPHOME_COMMIT = "3e3822e5541f3562fae64b29837b79b1027af674"
GUITION_URL = (
    "https://raw.githubusercontent.com/esphome/esphome/"
    + ESPHOME_COMMIT
    + "/esphome/components/mipi_dsi/models/guition.py"
)

DRIVER_FILES = [
    "src/chipguy_JC8012P4A1C_display.cpp",
    "src/chipguy_JC8012P4A1C_display.h",
    "src/esp_lcd_jd9365.c",
    "src/esp_lcd_jd9365.h",
    "src/esp_lcd_panel_dpi_bb.c",
    "src/esp_lcd_panel_dpi_bb.h",
    "src/esp_lcd_touch.c",
    "src/esp_lcd_touch.h",
    "src/gsl3680_fw.c",
    "src/gsl3680_fw.h",
    "src/gsl3680_touch.cpp",
    "src/gsl3680_touch.h",
    "src/gsl_point_id.c",
    "src/gsl_point_id.h",
    "src/pins_config.h",
]


def fetch_text(url: str) -> str:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "FamilyCalendar-PlatformIO/1.6.0"},
    )
    # PlatformIO's Python normally has a valid CA bundle.  Keep TLS verification
    # enabled; failing closed is preferable to silently downloading unverified code.
    with urllib.request.urlopen(req, timeout=45) as response:
        return response.read().decode("utf-8")


def fetch_bytes(url: str) -> bytes:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "FamilyCalendar-PlatformIO/1.6.0"},
    )
    with urllib.request.urlopen(req, timeout=45) as response:
        return response.read()


def stage_driver():
    SRC_DIR.mkdir(parents=True, exist_ok=True)
    for rel in DRIVER_FILES:
        dest = LIB_DIR / rel
        if not dest.exists():
            print(f"[1.6.0] Downloading pinned display driver file: {rel}")
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(fetch_bytes(DRIVER_BASE + rel))

    # Local library metadata keeps PlatformIO's dependency scanner deterministic.
    (LIB_DIR / "library.json").write_text(
        '{\n  "name": "JC8012P4A1C_display_2624",\n  "version": "1.1.0",\n  "build": {"srcDir": "src"}\n}\n',
        encoding="utf-8",
    )


def extract_v2_sequence(text: str):
    marker = 'DsiDriverChip(\n    "JC8012P4A1-V2"'
    start = text.find(marker)
    if start < 0:
        raise RuntimeError("Pinned ESPHome file does not contain JC8012P4A1-V2")

    seq_key = "initsequence=["
    seq_pos = text.find(seq_key, start)
    if seq_pos < 0:
        raise RuntimeError("Could not locate V2 initsequence")

    open_bracket = text.find("[", seq_pos)
    depth = 0
    close_bracket = None
    for i in range(open_bracket, len(text)):
        ch = text[i]
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                close_bracket = i
                break
    if close_bracket is None:
        raise RuntimeError("Could not parse V2 initsequence")

    sequence = ast.literal_eval(text[open_bracket:close_bracket + 1])
    if len(sequence) < 150:
        raise RuntimeError(f"V2 initsequence unexpectedly short: {len(sequence)}")
    return sequence


def c_table(sequence):
    lines = []
    for item in sequence:
        if not isinstance(item, tuple) or len(item) < 2:
            raise RuntimeError(f"Unexpected init item: {item!r}")
        cmd = int(item[0])
        data = [int(v) for v in item[1:]]
        data_text = ", ".join(f"0x{v:02X}" for v in data)
        lines.append(
            f"    {{0x{cmd:02X}, (uint8_t[]){{{data_text}}}, {len(data)}, 0}},"
        )
    return "\n".join(lines)


def patch_driver():
    profile = fetch_text(GUITION_URL)
    sequence = extract_v2_sequence(profile)

    c_path = SRC_DIR / "esp_lcd_jd9365.c"
    c_src = c_path.read_text(encoding="utf-8")
    table_re = re.compile(
        r"const jd9365_lcd_init_cmd_t vendor_specific_init_default\[\] = \{.*?\n\};\nconst size_t vendor_specific_init_default_size",
        re.S,
    )
    replacement = (
        "const jd9365_lcd_init_cmd_t vendor_specific_init_default[] = {\n"
        + c_table(sequence)
        + "\n};\nconst size_t vendor_specific_init_default_size"
    )
    c_src, count = table_re.subn(replacement, c_src, count=1)
    if count != 1:
        raise RuntimeError("Could not replace JD9365 vendor init table")
    c_path.write_text(c_src, encoding="utf-8")

    cpp_path = SRC_DIR / "chipguy_JC8012P4A1C_display.cpp"
    cpp = cpp_path.read_text(encoding="utf-8")
    required = {
        ".dpi_clock_freq_mhz = 60,": ".dpi_clock_freq_mhz = 70,",
        ".vsync_back_porch = 8,": ".vsync_back_porch = 10,",
    }
    for old, new in required.items():
        if old in cpp:
            cpp = cpp.replace(old, new, 1)
        elif new not in cpp:
            raise RuntimeError(f"Could not patch display timing: {old}")
    cpp_path.write_text(cpp, encoding="utf-8")

    # Patch the public timing macro as well, even though the class currently
    # constructs its DPI config directly.  This keeps the local library coherent.
    h_path = SRC_DIR / "esp_lcd_jd9365.h"
    h = h_path.read_text(encoding="utf-8")
    h = h.replace(".dpi_clock_freq_mhz = 60,", ".dpi_clock_freq_mhz = 70,")
    h = h.replace(".vsync_back_porch = 8,", ".vsync_back_porch = 10,")
    h_path.write_text(h, encoding="utf-8")

    marker = LIB_DIR / "FAMILY_CALENDAR_2624_PATCH.txt"
    marker.write_text(
        "Family Calendar 1.1.3\n"
        "Target: JC8012P4A1C_I_W_Y / SKU 10153001 (2624)\n"
        "JD9365 profile: ESPHome JC8012P4A1-V2\n"
        "PCLK: 70 MHz\n"
        "Lane rate: 1500 Mbps\n"
        "VSYNC: 4/10/20\n"
        f"Driver commit: {DRIVER_COMMIT}\n"
        f"ESPHome profile commit: {ESPHOME_COMMIT}\n",
        encoding="utf-8",
    )
    print(f"[1.6.0] Applied {len(sequence)}-command JD9365 2624/V2 init profile")
    print("[1.6.0] Display timing: 70 MHz, 2 lanes @ 1500 Mbps, VSYNC 4/10/20")


try:
    stage_driver()
    patch_driver()
except Exception as exc:
    print(f"\n[1.6.0] ERROR preparing JC8012P4A1C display driver: {exc}\n")
    raise
