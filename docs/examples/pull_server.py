"""TickrDisplay Pull URL example server (docs/INTEGRATION_EXAMPLE.md).

Serves ONE JSON object the device understands at  GET /tickr?token=<token>
Built from the Binance USDT-margined futures 24 h tickers: every pair whose
24 h change is at or above THRESHOLD_PCT, strongest first.

    pip install fastapi uvicorn requests
    TICKR_TOKEN=<token> THRESHOLD_PCT=10 uvicorn pull_server:app --host 0.0.0.0 --port 8000

    Device: Source = Custom JSON URL, URL = http://<server-host>:8000/tickr?token=<token>

GRID=1 sends the top four movers as a 2x2 tile frame instead of one text /
ticker frame (docs/API.md "Tile layouts"): one tile per coin, the cells of
coins below the threshold left empty (null). The server picks the tiles,
the device draws them.
"""
import os
import threading
import time

import requests
from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse

# EDIT: the shared secret; the device sends it as ?token=... in the URL.
TOKEN = os.environ.get("TICKR_TOKEN", "")
# EDIT: a pair must have moved at least this much (per cent, 24 h) to be listed.
THRESHOLD_PCT = float(os.environ.get("THRESHOLD_PCT", "10"))
# EDIT: how often the exchange is asked (seconds). The device polls on its own interval.
REFRESH_S = int(os.environ.get("REFRESH_S", "60"))
# EDIT: LED brightness 0-255 (255 = full). Thinking in per cent? round(pct * 255 / 100).
LED_BRIGHTNESS = max(0, min(255, int(os.environ.get("LED_BRIGHTNESS", "255"))))
# EDIT: any JSON API; change fetch_rows() to match its shape.
SOURCE_URL = "https://fapi.binance.com/fapi/v1/ticker/24hr"
# EDIT: GRID=1 -> a 2x2 of tiles (up to four coins), else one frame with the strongest mover.
GRID = os.environ.get("GRID", "0") == "1"
# EDIT: the tile style (GRID=1): look stack|row|ticker, size auto|l|m|s, invert true|false.
TILE_STYLE = {"look": "stack", "size": "auto"}

# Firmware limits (tickr_display/src/logic/payload.h, tiles.h): bytes, not characters.
TITLE_MAX, VALUE_MAX, CHANGE_MAX = 63, 127, 15
TILE_TITLE_MAX, TILE_VALUE_MAX = 15, 23

app = FastAPI()
_lock = threading.Lock()
_payload = {"title": "Binance movers", "value": "starting...", "alert": {"led": "000000"}}


def cut(text: str, limit: int) -> str:
    """Trim to the firmware limit; the panel fonts draw printable ASCII only."""
    text = text.encode("ascii", "replace").decode("ascii")
    return text if len(text) <= limit else text[: limit - 3] + "..."


def fetch_rows():
    """[(symbol, pct, last_price), ...] above the threshold, strongest first."""
    r = requests.get(SOURCE_URL, timeout=10, headers={"Accept": "application/json"})
    r.raise_for_status()
    rows = []
    for t in r.json():
        sym = str(t.get("symbol", ""))
        if not sym.endswith("USDT") or "_" in sym:   # EDIT: perpetuals quoted in USDT only
            continue
        try:
            pct = float(t["priceChangePercent"])
            last = str(t.get("lastPrice", ""))
        except (KeyError, TypeError, ValueError):
            continue
        if pct >= THRESHOLD_PCT:
            rows.append((sym[:-4], pct, last))
    rows.sort(key=lambda x: x[1], reverse=True)
    return rows


def tile_price(last: str) -> str:
    """A short price for a 148 px cell: the bottom row of a tile holds about
    7 characters of value next to a 4-5 character change (docs/API.md "Tile layouts")."""
    try:
        p = float(last)
    except ValueError:
        return last
    if p >= 1000:
        return f"{p:.0f}"           # 80000
    if p >= 1:
        return f"{p:.2f}"           # 185.20
    return f"{p:.3g}"               # 0.00237


def build_tiles(rows) -> dict:
    """GRID=1: the top four as a 2x2; cells without a mover stay empty (null).
    A tile is title / value / change (+ dir, derived from the sign); the device
    picks the font sizes. All-empty tiles draw a blank grid - still one object."""
    tiles = [None, None, None, None]
    for i, (sym, pct, last) in enumerate(rows[:4]):
        tiles[i] = {"title": cut(sym, TILE_TITLE_MAX), "value": cut(tile_price(last), TILE_VALUE_MAX),
                    "change": cut(f"+{pct:.0f}%", CHANGE_MAX)}
    return {
        "layout": "2x2",
        "style": TILE_STYLE,
        "tiles": tiles,
        "title": "Binance movers",            # the shelf's Showing line only
        "alert": {"led": "00FF00" if rows else "000000", "brightness": LED_BRIGHTNESS},
    }


def build_payload(rows) -> dict:
    """Always ONE object - never a list, never {} - even when there is nothing to show."""
    if GRID:
        return build_tiles(rows)
    if not rows:
        # EDIT: what the panel shows while nothing passes the threshold.
        return {"title": "Binance movers",
                "value": cut(f"nothing above +{THRESHOLD_PCT:g}%", VALUE_MAX),
                "alert": {"led": "000000"}}
    sym, pct, _ = rows[0]
    # EDIT: the text lines. `change` + `dir` switch the panel to the ticker look.
    return {
        "title": cut(f"{sym} +{pct:.1f}%", TITLE_MAX),
        "value": cut(" | ".join(f"{s} +{p:.1f}%" for s, p, _ in rows), VALUE_MAX),
        "change": cut(f"+{pct:.1f}%", CHANGE_MAX),
        "dir": 1,
        # EDIT: RRGGBB, beep|double_beep|long_beep|none. brightness is sticky on the device but
        # reset on boot (every battery wake-up is a boot), so it is sent in every payload.
        "alert": {"led": "00FF00", "sound": "double_beep", "brightness": LED_BRIGHTNESS},
    }


def refresh_loop():
    global _payload
    while True:
        try:
            p = build_payload(fetch_rows())
        except Exception as e:                      # the device must still get an object
            p = {"title": "Binance movers", "value": cut(f"source error: {e}", VALUE_MAX),
                 "alert": {"led": "FF0000"}}
        with _lock:
            _payload = p
        time.sleep(REFRESH_S)


threading.Thread(target=refresh_loop, daemon=True).start()


@app.get("/tickr")
def tickr(token: str = Query(default="")):
    if not TOKEN or token != TOKEN:
        return JSONResponse({"error": "unauthorized"}, status_code=401,
                            headers={"Cache-Control": "no-store"})
    with _lock:
        body = dict(_payload)
    return JSONResponse(body, headers={"Cache-Control": "no-store"})
