#!/usr/bin/env python3
"""
SFT ESP32 — Tile & Assets Downloader
=====================================
Downloads OSM map tiles for Lago di Garda and the Leaflet JS/CSS library
into the `data/` directory, ready to be flashed to ESP32 LittleFS.

Run ONCE before uploading the filesystem:
    python3 download_tiles.py

Then upload everything to the ESP32:
    pio run -e no_memory --target uploadfs   # upload LittleFS image
    pio run -e no_memory --target upload     # upload firmware

Requirements: Python 3.8+  (no extra packages needed)
"""

import math
import os
import sys
import time
import urllib.request
import urllib.error

# ─────────────────────────────────────────────────────────────────────────────
# CONFIGURATION
# ─────────────────────────────────────────────────────────────────────────────

# Lago di Garda bounding box (with a small margin around the lake)
LAT_MIN = 45.43   # Peschiera del Garda (southern tip)
LAT_MAX = 45.92   # Riva del Garda (northern tip)
LON_MIN = 10.43   # Western shore
LON_MAX = 10.81   # Eastern shore

# Zoom levels to cache
# z10 ≈ 4–6 tiles   (overview)
# z11 ≈ 8–12 tiles  (lake visible)
# z12 ≈ 24–40 tiles (navigation)
# z13 ≈ 80–150 tiles (detail)
ZOOM_LEVELS = [10, 11, 12, 13]

# Directories (relative to this script → no_memory/)
DATA_DIR  = "data"
TILES_DIR = os.path.join(DATA_DIR, "tiles")

# OSM tile endpoint — respect the usage policy!
TILE_URL  = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

# Leaflet 1.9.4
LEAFLET_VER    = "1.9.4"
LEAFLET_JS_URL = f"https://unpkg.com/leaflet@{LEAFLET_VER}/dist/leaflet.js"
LEAFLET_CSS_URL= f"https://unpkg.com/leaflet@{LEAFLET_VER}/dist/leaflet.css"

# Polite rate-limiting (OSM requests ≥ 1 req/s from non-cached clients)
TILE_DELAY = 0.5   # seconds between tile requests

USER_AGENT = (
    "SFT-Telemetry-ESP32/1.0 "
    "(Sapienza Foiling Team; educational offline tile cache; "
    "contact: foilingteam@uniroma1.it)"
)

# ─────────────────────────────────────────────────────────────────────────────
# TILE MATH  (Slippy Map / OpenStreetMap convention)
# ─────────────────────────────────────────────────────────────────────────────

def deg2tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    """Convert geographic coordinates to tile (x, y) at the given zoom."""
    n   = 2 ** zoom
    x   = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y   = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return x, y

# ─────────────────────────────────────────────────────────────────────────────
# DOWNLOAD HELPERS
# ─────────────────────────────────────────────────────────────────────────────

def fetch(url: str, dest: str, headers: dict | None = None, retries: int = 3) -> bool:
    """Download *url* to *dest*.  Returns True on success."""
    h = {"User-Agent": USER_AGENT}
    if headers:
        h.update(headers)
    for attempt in range(1, retries + 1):
        try:
            req = urllib.request.Request(url, headers=h)
            with urllib.request.urlopen(req, timeout=20) as resp:
                data = resp.read()
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
            return True
        except urllib.error.HTTPError as e:
            if e.code == 429:
                wait = 5 * attempt
                print(f"  Rate-limited (429) — waiting {wait}s …", flush=True)
                time.sleep(wait)
            else:
                print(f"  HTTP {e.code}: {e.reason}", flush=True)
                return False
        except Exception as e:
            if attempt < retries:
                time.sleep(2 * attempt)
            else:
                print(f"  Error: {e}", flush=True)
                return False
    return False

# ─────────────────────────────────────────────────────────────────────────────
# LEAFLET
# ─────────────────────────────────────────────────────────────────────────────

def download_leaflet() -> None:
    print("\n━━━  Leaflet JS/CSS  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    os.makedirs(DATA_DIR, exist_ok=True)

    for url, fname in [(LEAFLET_JS_URL,  "leaflet.min.js"),
                       (LEAFLET_CSS_URL, "leaflet.min.css")]:
        dest = os.path.join(DATA_DIR, fname)
        if os.path.exists(dest) and os.path.getsize(dest) > 0:
            print(f"  SKIP  {fname}  ({os.path.getsize(dest)//1024} KB)")
            continue
        print(f"  GET   {fname} … ", end="", flush=True)
        if fetch(url, dest):
            print(f"OK  ({os.path.getsize(dest)//1024} KB)")
        else:
            print("FAILED  ← check your internet connection")

# ─────────────────────────────────────────────────────────────────────────────
# TILES
# ─────────────────────────────────────────────────────────────────────────────

def download_tiles() -> None:
    print("\n━━━  OSM Tiles — Lago di Garda  ━━━━━━━━━━━━━━━━━")
    print(f"  Bounding box: lat [{LAT_MIN}, {LAT_MAX}]  lon [{LON_MIN}, {LON_MAX}]")
    print(f"  Zoom levels : {ZOOM_LEVELS}\n")

    total_ok = total_skip = total_fail = total_bytes = 0

    for z in ZOOM_LEVELS:
        # In Slippy Map, HIGHER latitude → LOWER y index
        x_min, y_max = deg2tile(LAT_MIN, LON_MIN, z)
        x_max, y_min = deg2tile(LAT_MAX, LON_MAX, z)
        n_tiles = (x_max - x_min + 1) * (y_max - y_min + 1)

        print(f"  Zoom {z}: x=[{x_min}…{x_max}]  y=[{y_min}…{y_max}]  → {n_tiles} tiles")

        for x in range(x_min, x_max + 1):
            for y in range(y_min, y_max + 1):
                dest = os.path.join(TILES_DIR, str(z), str(x), f"{y}.png")

                if os.path.exists(dest) and os.path.getsize(dest) > 0:
                    total_skip += 1
                    continue

                url = TILE_URL.format(z=z, x=x, y=y)
                sys.stdout.write(f"    {z}/{x}/{y}.png … ")
                sys.stdout.flush()

                if fetch(url, dest):
                    size = os.path.getsize(dest)
                    total_bytes += size
                    total_ok    += 1
                    print(f"OK ({size // 1024} KB)")
                else:
                    total_fail += 1
                    print("FAILED")

                time.sleep(TILE_DELAY)

    print(f"\n  Downloaded : {total_ok} tiles  ({total_bytes / 1024 / 1024:.2f} MB)")
    print(f"  Skipped    : {total_skip} tiles  (already on disk)")
    print(f"  Failed     : {total_fail} tiles")

# ─────────────────────────────────────────────────────────────────────────────
# SUMMARY
# ─────────────────────────────────────────────────────────────────────────────

def print_summary() -> None:
    total = sum(
        os.path.getsize(os.path.join(root, f))
        for root, _, files in os.walk(DATA_DIR)
        for f in files
    )
    fs_mb = total / 1024 / 1024
    cap_mb = 6.0  # LittleFS partition size in partitions.csv

    print("\n━━━  data/ Summary  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"  Total size : {fs_mb:.2f} MB  /  {cap_mb:.1f} MB partition")
    bar_len = 30
    filled  = int(bar_len * fs_mb / cap_mb)
    bar     = "█" * filled + "░" * (bar_len - filled)
    pct     = fs_mb / cap_mb * 100
    color   = "\033[91m" if pct > 90 else "\033[93m" if pct > 70 else "\033[92m"
    print(f"  [{color}{bar}\033[0m] {pct:.0f}%")

    if fs_mb > cap_mb:
        print("\n  ⚠️  WARNING: data/ exceeds 6 MB partition capacity!")
        print("     → Reduce zoom levels or shrink the bounding box.")
    else:
        print("\n  ✅  Fits in LittleFS partition — ready to upload.")

    print("\n━━━  Next Steps  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print("  1. Upload filesystem :  pio run -e no_memory --target uploadfs")
    print("  2. Upload firmware   :  pio run -e no_memory --target upload")
    print("  3. Connect to Wi-Fi  :  SFT_telemetry / la password configurata in secrets.h")
    print("  4. Open browser      :  http://192.168.4.1\n")

# ─────────────────────────────────────────────────────────────────────────────
# ENTRY POINT
# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    # Script should be run from the no_memory/ directory
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    print("╔══════════════════════════════════════════════════╗")
    print("║  SFT ESP32 — Tile & Assets Downloader           ║")
    print("║  Sapienza Foiling Team                          ║")
    print("╚══════════════════════════════════════════════════╝")

    download_leaflet()
    download_tiles()
    print_summary()
