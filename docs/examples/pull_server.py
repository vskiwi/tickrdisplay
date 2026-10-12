"""TickrDisplay Pull URL example server (docs/INTEGRATION_EXAMPLE.md).

Serves ONE JSON object the device understands at  GET /tickr?token=<token>
Built from the Binance USDT-margined futures 24 h tickers: every pair whose
24 h change is at or above THRESHOLD_PCT, strongest first.

    pip install fastapi uvicorn requests
    TICKR_TOKEN=<token> THRESHOLD_PCT=10 uvicorn pull_server:app --host 0.0.0.0 --port 8000

Device: Source = Custom JSON URL, URL = http://<server-host>:8000/tickr?token=<token>
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

# Firmware limits (tickr_display/src/logic/payload.h): bytes, not characters.
TITLE_MAX, VALUE_MAX, CHANGE_MAX = 63, 127, 15

app = FastAPI()
_lock = threading.Lock()
_payload = {"title": "Binance movers", "value": "starting...", "alert": {"led": "000000"}}


def cut(text: str, limit: int) -> str:
    """Trim to the firmware limit; the panel fonts draw printable ASCII only."""
    text = text.encode("ascii", "replace").decode("ascii")
    return text if len(text) <= limit else text[: limit - 3] + "..."


def fetch_rows():
    """[(symbol, pct), ...] above the threshold, strongest first."""
    r = requests.get(SOURCE_URL, timeout=10, headers={"Accept": "application/json"})
    r.raise_for_status()
    rows = []
    for t in r.json():
        sym = str(t.get("symbol", ""))
        if not sym.endswith("USDT") or "_" in sym:   # EDIT: perpetuals quoted in USDT only
            continue
        try:
            pct = float(t["priceChangePercent"])
        except (KeyError, TypeError, ValueError):
            continue
        if pct >= THRESHOLD_PCT:
            rows.append((sym[:-4], pct))
    rows.sort(key=lambda x: x[1], reverse=True)
    return rows


def build_payload(rows) -> dict:
    """Always ONE object - never a list, never {} - even when there is nothing to show."""
    if not rows:
        # EDIT: what the panel shows while nothing passes the threshold.
        return {"title": "Binance movers",
                "value": cut(f"nothing above +{THRESHOLD_PCT:g}%", VALUE_MAX),
                "alert": {"led": "000000"}}
    sym, pct = rows[0]
    # EDIT: the text lines. `change` + `dir` switch the panel to the ticker look.
    return {
        "title": cut(f"{sym} +{pct:.1f}%", TITLE_MAX),
        "value": cut(" | ".join(f"{s} +{p:.1f}%" for s, p in rows), VALUE_MAX),
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
