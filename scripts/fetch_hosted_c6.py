Import("env")

from pathlib import Path
import subprocess
import urllib.request

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
FW_DIR = PROJECT_DIR / "data" / "hosted"
FW_PATH = FW_DIR / "esp32c6-v2.12.3.bin"
FW_URL = "https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.3.bin"
MIN_SIZE = 64 * 1024
MAX_SIZE = 4 * 1024 * 1024


def valid_firmware(path: Path) -> bool:
    if not path.exists():
        return False
    size = path.stat().st_size
    if size < MIN_SIZE or size > MAX_SIZE:
        return False
    # ESP application images start with 0xE9.  This catches HTML/error pages
    # accidentally saved under the .bin filename.
    with path.open("rb") as fh:
        return fh.read(1) == b"\xE9"


def download_with_urllib() -> None:
    req = urllib.request.Request(
        FW_URL,
        headers={"User-Agent": "FamilyCalendar-PlatformIO/1.6.0"},
    )
    with urllib.request.urlopen(req, timeout=90) as response:
        data = response.read()
    FW_PATH.write_bytes(data)


def download_with_curl() -> None:
    subprocess.run(
        ["curl", "-fL", "--retry", "3", "--connect-timeout", "20", "-o", str(FW_PATH), FW_URL],
        check=True,
    )


FW_DIR.mkdir(parents=True, exist_ok=True)

if not valid_firmware(FW_PATH):
    if FW_PATH.exists():
        FW_PATH.unlink()
    print(f"[1.6.0] Downloading ESP32-C6 ESP-Hosted firmware 2.12.3 from Espressif...")
    try:
        download_with_urllib()
    except Exception as first_error:
        print(f"[1.6.0] Python download failed ({first_error}); trying curl...")
        try:
            download_with_curl()
        except Exception as second_error:
            raise RuntimeError(
                "Could not download the official ESP32-C6 hosted firmware. "
                f"Download {FW_URL} manually and save it as {FW_PATH}. "
                f"urllib error: {first_error}; curl error: {second_error}"
            )

if not valid_firmware(FW_PATH):
    raise RuntimeError(
        f"Downloaded ESP32-C6 firmware at {FW_PATH} does not look like a valid ESP image."
    )

print(f"[1.6.0] ESP32-C6 hosted firmware ready: {FW_PATH.name} ({FW_PATH.stat().st_size} bytes)")
