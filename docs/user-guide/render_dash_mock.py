#!/usr/bin/env python3
"""Render simulated cyd-horizon screens to PNGs for the printable user guide.

Outputs three 960x720 screenshots:

- DEVELOPER-USERGUIDE-dashboard.png — flight-overhead view (or idle view when
  no plane is near): callsign, airline, route, live radar with the plane-icon
  blips, cyan dotted ground track, and the light-grey projection ray.
- DEVELOPER-USERGUIDE-idle.png — the idle weather view with pool temperature.
- DEVELOPER-USERGUIDE-wxgraph.png — the Weather Temperature History graph
  (Day view) plotted from the last 24h of real temps.
- DEVELOPER-USERGUIDE-ftracker.png — Settings > Flight Tracker, page 1.
- DEVELOPER-USERGUIDE-alarms.png — the Alarms > N editor (time steppers,
  weekday toggles, LED preset, footer buttons).

Fetches real data so the mock looks like the real thing:

- OpenSky OAuth + /states/all (needs OPENSKY_CLIENT_ID/SECRET in
  cyd-horizon/.env) for live aircraft + the CRP credit readout.
- adsb.lol vrs-standing-data for the featured flight's route (no key).
- OpenSky /flights/aircraft + /tracks/all for the CRL/CFT credit readouts and
  the ground-track polyline. Planes are scanned until one has a track with
  points near us; --monitor keeps polling for a while if none does yet.
- open-meteo (no key) for weather; location comes from LAT/LON in .env, else
  an ip-api.com guess (same as the device's first boot).
- Govee Open API (GOVEE_KEY) for the pool temperature.

Every fetch is optional: missing data just degrades the mock, it never fails
the render. PNGs via cairosvg.

By default the fetched data is read from / written to mock_data.json in this
folder, so re-rendering the PDF doesn't hit the APIs every time. Pass
--refresh to pull fresh data (which re-saves the cache).

Usage:
    cyd-horizon/.venv/bin/python docs/user-guide/render_dash_mock.py [--monitor SECONDS] [--refresh]
"""

import json
import math
import pathlib
import sys
import time
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent   # docs/user-guide/
REPO = ROOT.parents[1]                            # repo root
OUT_DASH = ROOT / "DEVELOPER-USERGUIDE-dashboard.png"
OUT_IDLE = ROOT / "DEVELOPER-USERGUIDE-idle.png"
OUT_GRAPH = ROOT / "DEVELOPER-USERGUIDE-wxgraph.png"
OUT_FTRK = ROOT / "DEVELOPER-USERGUIDE-ftracker.png"
OUT_ALRM = ROOT / "DEVELOPER-USERGUIDE-alarms.png"
OUT_DATA = ROOT / "mock_data.json"
ENV = REPO / "cyd-horizon" / ".env"

UA = {"User-Agent": "cyd-horizon-userguide/1.0"}
RADIUS_MI = 3.5          # device default radar radius
RADAR_CX, RADAR_CY, RADAR_R = 235, 155, 48

# Screen palette approximating the device's themed colors (default blue
# theme): header bands take the theme color; ordinary + Back buttons are the
# theme nudged lighter (btnCol); destructive buttons are red; the history
# graphs draw on the button color with a 75%-to-white theme line and a
# complementary (yellow) average line.
TH   = "#000080"   # theme / header band
BTN  = "#4040a0"   # btnCol() — theme +25% toward white
DGR  = "#c80000"   # dangerCol() — destructive buttons
GLN  = "#bfbfe0"   # graphLineCol() — theme +75% toward white
GAVG = "#ffffa0"   # graphAvgCol() — theme complement (yellow) +75% to white
LAV  = "#e6beff"   # TFT_LAVENDER — the pool temperature row

# Fixed clock/date shown in the header of every screenshot — a real flight/
# weather snapshot, but a stable timestamp for the printed guide.
MOCK_DT = __import__("datetime").datetime(2026, 9, 13, 16, 23)


def load_env():
    env = {}
    if ENV.exists():
        for line in ENV.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                env[k.strip()] = v.strip()
    return env


def get_json(url, headers=None, data=None, timeout=10):
    req = urllib.request.Request(
        url, data=data, headers={**UA, **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read()), r.headers
    except urllib.error.HTTPError as e:
        if e.code == 429:   # credits exhausted: surface it, don't drop the bucket
            return None, e.headers
        raise


def haversine_mi(lat1, lon1, lat2, lon2):
    r = 3958.8
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = math.radians(lat2 - lat1), math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def dxdy_mi(lat, lon, plat, plon):
    """East/north offsets in miles, same equirectangular approx the device uses."""
    dy = (plat - lat) * 69.0
    dx = (plon - lon) * math.cos(math.radians(lat)) * 69.0
    return dx, dy


def opensky_token(env):
    cid, sec = env.get("OPENSKY_CLIENT_ID"), env.get("OPENSKY_CLIENT_SECRET")
    if not (cid and sec):
        return None
    try:
        body = urllib.parse.urlencode({
            "grant_type": "client_credentials",
            "client_id": cid, "client_secret": sec}).encode()
        j, _ = get_json(
            "https://auth.opensky-network.org/auth/realms/"
            "opensky-network/protocol/openid-connect/token", data=body)
        return j.get("access_token")
    except Exception:
        return None


def poll_planes(lat, lon, token):
    """/states/all in the device's bbox (2x radius, min 8mi). -> (planes, crp)"""
    bbox = max(RADIUS_MI * 2.0, 8.0)
    dla = bbox / 69.0
    dlo = dla / math.cos(math.radians(lat))
    url = ("https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f"
           "&lamax=%.4f&lomax=%.4f" % (lat - dla, lon - dlo, lat + dla, lon + dlo))
    j, hdrs = get_json(url, {"Authorization": f"Bearer {token}"})
    crp = hdrs.get("X-Rate-Limit-Remaining", "?")
    planes = []
    for s in (j or {}).get("states") or []:
        if s[6] is None or s[5] is None or s[8]:
            continue  # no position or on the ground
        dx, dy = dxdy_mi(lat, lon, s[6], s[5])
        planes.append({
            "icao24": s[0], "cs": (s[1] or "").strip(),
            "lat": s[6], "lon": s[5], "dx": dx, "dy": dy,
            "altFt": int((s[7] or s[13] or 0) * 3.28084),
            "mph": int((s[9] or 0) * 1.94384 * 1.15078),
            "hdg": s[10] if s[10] is not None else -1,
            "dist": haversine_mi(lat, lon, s[6], s[5]),
        })
    planes.sort(key=lambda p: p["dist"])
    return planes, crp


def fetch_adsb_route(callsign):
    """adsb.lol callsign route -> {"o","d"} or None."""
    j, _ = get_json(
        "https://vrs-standing-data.adsb.lol/routes/%s/%s.json"
        % (callsign[:2], callsign))
    aps = (j or {}).get("_airports") or []
    if len(aps) < 2:
        return None

    def ap(a):
        code = a.get("icao", "")
        if a.get("iata"):
            code += " | " + a["iata"]
        return {"code": code, "city": a.get("location", "")}
    return {"o": ap(aps[0]), "d": ap(aps[-1])}


def fetch_track(lat, lon, icao24, token):
    """/tracks/all -> (track_pts[(dx,dy)], bearing_deg, cft).

    Mirrors fetchTrack(): keeps points within ~2x radar range (min 8mi) of the
    observer, newest 64, and the newest near point's true-track as the bearing.
    """
    url = "https://opensky-network.org/api/tracks/all?icao24=%s&time=0" % icao24
    j, hdrs = get_json(url, {"Authorization": f"Bearer {token}"})
    cft = hdrs.get("X-Rate-Limit-Remaining", "?")
    pts, bearing = [], -1.0
    max_range = max(RADIUS_MI * 2.0, 8.0)
    for p in ((j or {}).get("path") or []):
        plat, plon = p[1], p[2]
        if plat is None or plon is None:
            continue
        dx, dy = dxdy_mi(lat, lon, plat, plon)
        if math.hypot(dx, dy) > max_range:
            continue
        pts.append((dx, dy))
        if p[4] is not None:
            bearing = float(p[4])
    return pts[-64:], bearing, cft


def pick_featured(d, token, monitor_sec):
    """Find a plane to feature: prefer one with a real track near us plus a
    callsign route. Retries (re-polling states) until monitor_sec expires."""
    deadline = time.time() + monitor_sec
    while True:
        try:
            d["planes"], d["crp"] = poll_planes(d["lat"], d["lon"], token)
        except Exception:
            d["planes"] = []
        fallback = None   # first plane with a usable track but no route
        for p in d["planes"][:6]:
            try:
                pts, bearing, cft = fetch_track(
                    d["lat"], d["lon"], p["icao24"], token)
                d["cft"] = cft
            except Exception:
                continue
            if len(pts) < 2:
                continue
            try:
                rt = fetch_adsb_route(p["cs"]) if p["cs"] else None
            except Exception:
                rt = None
            if rt and rt["o"]["code"] != rt["d"]["code"]:
                d["track"], d["bearing"] = pts, bearing
                d["feat"], d["route"] = p, rt
                return
            if fallback is None:
                fallback = (p, pts, bearing, rt)
        if fallback:
            p, d["track"], d["bearing"], d["route"] = fallback
            d["feat"] = p
            # A same-airport route (local/training flights) reads like a bug
            # in the guide — show "No route data" instead.
            if d["route"] and d["route"]["o"]["code"] == d["route"]["d"]["code"]:
                d["route"] = None
            return
        if time.time() >= deadline:
            break
        time.sleep(min(35, max(0, deadline - time.time())))

    # No track found: still feature the closest plane with a distinct route.
    if not d["planes"]:
        return
    for p in d["planes"][:4]:
        if not p["cs"]:
            continue
        try:
            rt = fetch_adsb_route(p["cs"])
        except Exception:
            continue
        if rt and rt["o"]["code"] != rt["d"]["code"]:
            d["feat"], d["route"] = p, rt
            return
    d["feat"] = d["planes"][0]


def fetch_all(env, monitor_sec):
    """Best-effort fetch of everything the mock can use; all keys optional."""
    d = {"planes": [], "crp": "?", "crl": "?", "cft": "?",
         "route": None, "track": [], "bearing": -1.0, "feat": None,
         "wx": None, "wx_hist": [], "pool": None}

    lat = float(env.get("LAT", "0") or 0)
    lon = float(env.get("LON", "0") or 0)
    if not lat and not lon:
        try:
            j, _ = get_json("http://ip-api.com/json/")
            lat, lon = float(j["lat"]), float(j["lon"])
        except Exception:
            lat, lon = 33.0, -97.0
    d["lat"], d["lon"] = lat, lon

    token = opensky_token(env)
    if token:
        pick_featured(d, token, monitor_sec)
        if d["feat"]:
            try:
                now = int(time.time())
                url = ("https://opensky-network.org/api/flights/aircraft"
                       "?icao24=%s&begin=%d&end=%d"
                       % (d["feat"]["icao24"], now - 4 * 3600, now + 60))
                _, hdrs = get_json(url, {"Authorization": f"Bearer {token}"})
                d["crl"] = hdrs.get("X-Rate-Limit-Remaining", "0")
            except Exception:
                pass

    # Weather for the idle view + last-24h temps for the graph (open-meteo)
    try:
        url = ("https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
               "weather_code,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
               "&hourly=temperature_2m&past_days=1&forecast_days=8"
               "&daily=temperature_2m_max,temperature_2m_min,"
               "precipitation_probability_max,weather_code,sunrise,sunset"
               "&temperature_unit=fahrenheit&wind_speed_unit=kn"
               "&timezone=auto&timeformat=unixtime"
               % (lat, lon))
        d["wx"], _ = get_json(url)
        hr = d["wx"].get("hourly", {})
        d["wx_hist"] = [(t, v) for t, v in
                        zip(hr.get("time") or [], hr.get("temperature_2m") or [])
                        if v is not None]
    except Exception:
        pass

    # Govee pool temp (first thermometer on the account, "pool" name preferred)
    key = env.get("GOVEE_KEY")
    if key:
        try:
            j, _ = get_json(
                "https://openapi.api.govee.com/router/api/v1/user/devices",
                {"Govee-API-Key": key})
            thermos = []
            for x in (j or {}).get("data") or []:
                caps = x.get("capabilities") or []
                if ("thermometer" in (x.get("type") or "")
                        or any(c.get("instance") == "sensorTemperature"
                               for c in caps)):
                    thermos.append(x)
            pool = next((x for x in thermos if "pool" in
                         (x.get("deviceName") or "").lower()),
                        thermos[0] if thermos else None)
            if pool:
                body = json.dumps({"requestId": "pool-1", "payload": {
                    "sku": pool["sku"], "device": pool["device"]}}).encode()
                st, _ = get_json(
                    "https://openapi.api.govee.com/router/api/v1/device/state",
                    {"Govee-API-Key": key,
                     "Content-Type": "application/json"}, data=body)
                for cap in st["payload"]["capabilities"]:
                    if cap["instance"] == "sensorTemperature":
                        d["pool"] = float(cap["state"]["value"])
                        break
        except Exception:
            pass
    return d


# ---- tiny SVG helpers (320x240 canvas matching the CYD) ----
W, H = 320, 240
FONT = "Menlo, 'Courier New', monospace"
svg = []


def rect(x, y, w, h, fill, stroke=None, sw=0, rx=0):
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}"'
    if rx:
        s += f' rx="{rx}"'
    if stroke:
        s += f' stroke="{stroke}" stroke-width="{sw}"'
    svg.append(s + "/>")


def txt(x, y, size, fill, s, anchor="start"):
    svg.append(f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}"'
               f' fill="{fill}" text-anchor="{anchor}">{s}</text>')


def circle(cx, cy, r, stroke=None, sw=1, fill="none"):
    f = f' fill="{fill}"' if fill != "none" else ""
    svg.append(f'<circle cx="{cx}" cy="{cy}" r="{r}"'
               f' stroke="{stroke}" stroke-width="{sw}"{f}/>')


def line(x0, y0, x1, y1, stroke, sw=1, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    svg.append(f'<line x1="{x0}" y1="{y0}" x2="{x1}" y2="{y1}"'
               f' stroke="{stroke}" stroke-width="{sw}"{d}/>')


def tri(p0, p1, p2, fill, stroke="#000"):
    pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in (p0, p1, p2))
    st = f' stroke="{stroke}" stroke-width="0.75"' if stroke else ""
    svg.append(f'<polygon points="{pts}" fill="{fill}"{st}/>')


def dashed(x0, y0, x1, y1, col, dash, gap, keepouts=()):
    """Device's drawDottedLine + keep-out clipping: emit each dash segment,
    skipping any whose midpoint lands inside a keep-out rect."""
    dx, dy = x1 - x0, y1 - y0
    ln = math.hypot(dx, dy)
    if ln < 0.5:
        return
    ux, uy = dx / ln, dy / ln
    t = 0.0
    while t < ln:
        a, b = t, min(t + dash, ln)
        mx, my = x0 + ux * (a + b) / 2, y0 + uy * (a + b) / 2
        if not any(rx0 <= mx <= rx1 and ry0 <= my <= ry1
                   for rx0, ry0, rx1, ry1 in keepouts):
            line(x0 + ux * a, y0 + uy * a, x0 + ux * b, y0 + uy * b, col, 1)
        t += dash + gap


AIRLINES = {
    "AAL": "American Airlines", "DAL": "Delta Air Lines", "UAL": "United Airlines",
    "SWA": "Southwest Airlines", "JBU": "JetBlue Airways", "ASA": "Alaska Airlines",
    "SKW": "SkyWest Airlines", "ENY": "Envoy Air", "RPA": "Republic Airways",
    "ASH": "Mesa Airlines", "PDT": "Piedmont Airlines", "EDV": "Endeavor Air",
    "GJS": "GoJet Airlines", "FDX": "FedEx Express", "UPS": "UPS Airlines",
    "GTI": "Atlas Air", "ABX": "ABX Air", "NKS": "Spirit Airlines",
    "FFT": "Frontier Airlines", "EJA": "NetJets", "BAW": "British Airways",
    "DLH": "Lufthansa", "AFR": "Air France", "KLM": "KLM", "QFA": "Qantas",
    "ANA": "ANA", "JAL": "Japan Airlines", "ACA": "Air Canada", "WJA": "WestJet",
    "AMX": "Aeromexico", "VOI": "Volaris", "JZA": "Air Canada Jazz",
    "QTR": "Qatar Airways", "UAE": "Emirates", "THY": "Turkish Airlines",
    "KAL": "Korean Air", "ETH": "Ethiopian Airlines", "VIR": "Virgin Atlantic",
    "JIA": "PSA Airlines", "AWI": "Air Wisconsin", "QXE": "Horizon Air",
    "AAY": "Allegiant Air", "SCX": "Sun Country", "HAL": "Hawaiian Airlines",
}

# Keep-out rects (x0,y0,x1,y1) matching blipBlocked()/drawDottedLineSafe zones.
KO_HEADER = (-11, -11, 331, 44)
KO_COG = (285, 189, 331, 224)
KO_TEXT = (-11, 23, 182, 219)
KO_LOGO = (211, 23, 331, 104)


def header(d):
    clock = MOCK_DT.strftime("%I:%M")       # matches fmtClock() on the device
    date = MOCK_DT.strftime("%a %b ") + str(MOCK_DT.day)
    rect(0, 0, W, 36, TH)
    txt(4, 16, 11, "#fff", date)
    txt(96, 24, 22, "#fff", clock)
    # AM/PM marker stacked right of the time: AM top slot, PM bottom slot.
    pm = MOCK_DT.hour >= 12
    txt(170, 30 if pm else 13, 8, "#fff", MOCK_DT.strftime("%p"))
    # Alarm bell in the inactive slot, like the device draws when an alarm is
    # enabled (bell glyph: triangle body, rim, clapper).
    bx, by = 170, 5 if pm else 23
    tri((bx + 4, by), (bx, by + 6), (bx + 8, by + 6), "#fd0")
    rect(bx, by + 6, 9, 2, "#fd0")
    rect(bx + 3, by + 8, 3, 2, "#fd0")
    for i, (lbl, v) in enumerate((("CRP:", d["crp"]), ("CRL:", d["crl"]),
                                  ("CFT:", d["cft"]))):
        # Same tiers as the device: grey healthy, yellow <500 (or "?"), pink <50
        try:
            col = "#f9c" if int(v) < 50 else ("#ff0" if int(v) < 500 else "#ccc")
        except (TypeError, ValueError):
            col = "#ff0"
        txt(202, 10 + i * 10, 8, "#fff", lbl)
        txt(228, 10 + i * 10, 8, col, str(v))


def cog():
    # Bottom-right, matching drawCog(): dark-grey disc, 6 light-grey spokes,
    # black hub.
    cx, cy, r = 310, 215, 9
    circle(cx, cy, r, fill="#7b7d7b")
    for i in range(6):
        a = i * math.pi / 3.0
        line(cx + r * 0.6 * math.cos(a), cy + r * 0.6 * math.sin(a),
             cx + r * 1.3 * math.cos(a), cy + r * 1.3 * math.sin(a), "#ccc", 1)
    circle(cx, cy, 4, fill="#000")


def plane_icon(px, py, hdg, color, scale=1.0):
    """Port of drawPlaneIcon(): two-triangle silhouette along the heading
    (0=north, clockwise). Dot fallback when heading is unknown."""
    if hdg < 0:
        r = max(1, round(3.0 * scale))
        circle(px, py, r, "#000", 0.75, fill=color)
        return
    rad = math.radians(hdg)
    fx, fy = math.sin(rad), -math.cos(rad)   # forward
    rx, ry = math.cos(rad), math.sin(rad)    # right

    def pt(f, r):
        return (px + fx * f + rx * r, py + fy * f + ry * r)

    s = scale
    tri(pt(5.0 * s, 0), pt(-1.4 * s, -4.5 * s), pt(-1.4 * s, 4.5 * s), color)
    tri(pt(-5.5 * s, 0), pt(-1.4 * s, -1.7 * s), pt(-1.4 * s, 1.7 * s), color)


def blip_blocked(px, py):
    """Same zones the device's blipBlocked() protects (padded by kBlipR=11)."""
    if py <= 44 or py >= 213:
        return True
    if -11 <= px <= 182 and 23 <= py <= 219:
        return True   # flight-info text
    if 211 <= px <= 331 and 23 <= py <= 104:
        return True   # airline logo area
    if 285 <= px <= 331 and 189 <= py <= 224:
        return True   # settings cog
    return False


def radar(d):
    """Dashboard radar: rings, ground track, projection, blips, center dot."""
    cx, cy, r = RADAR_CX, RADAR_CY, RADAR_R
    scale = r / RADIUS_MI
    circle(cx, cy, r, "#7b7d7b")
    circle(cx, cy, r / 2, "#7b7d7b")
    line(cx - r, cy, cx + r, cy, "#7b7d7b")
    line(cx, cy - r, cx, cy + r, "#7b7d7b")
    txt(cx - 12, cy + r + 18, 11, "#ccc", f"{round(RADIUS_MI)}mi")

    # Ground track: cyan dotted polyline over the real /tracks points
    # (clipped against the header band and cog only, like CLIP_DYN_COG).
    ko_track = (KO_HEADER, KO_COG)
    pts = [(cx + dx * scale, cy - dy * scale) for dx, dy in d["track"]]
    for i in range(1, len(pts)):
        dashed(*pts[i - 1], *pts[i], "#0ff", 3, 3, ko_track)
    # Extend the track's oldest leg to the top of the black area so the path
    # visibly enters from the screen edge instead of stopping mid-screen.
    if len(pts) >= 2:
        ux, uy = pts[0][0] - pts[1][0], pts[0][1] - pts[1][1]
        n = math.hypot(ux, uy)
        if n > 1e-6:
            ux, uy = ux / n, uy / n
            t = 1e9
            if uy < -1e-4:
                t = min(t, (37 - pts[0][1]) / uy)   # top of the black area
            elif uy > 1e-4:
                t = min(t, (239 - pts[0][1]) / uy)
            if ux > 1e-4:
                t = min(t, (319 - pts[0][0]) / ux)
            elif ux < -1e-4:
                t = min(t, -pts[0][0] / ux)
            if 0 < t < 1e9:
                dashed(pts[0][0], pts[0][1], pts[0][0] + ux * t,
                       pts[0][1] + uy * t, "#0ff", 3, 3, ko_track)

    # Projection ray: light-grey dots from the track end (or the plane's
    # position + heading) to the screen edge, clipped against all UI zones.
    feat = d["feat"]
    if feat:
        if pts and d["bearing"] >= 0:
            sx, sy = pts[-1]
            rad = math.radians(d["bearing"])
        elif feat["hdg"] >= 0:
            sx = cx + feat["dx"] * scale
            sy = cy - feat["dy"] * scale
            rad = math.radians(feat["hdg"])
        else:
            sx = sy = rad = None
        if sx is not None:
            ux, uy = math.sin(rad), -math.cos(rad)
            t = 1e9
            if ux > 1e-4:
                t = min(t, (319 - sx) / ux)
            elif ux < -1e-4:
                t = min(t, -sx / ux)
            if uy > 1e-4:
                t = min(t, (239 - sy) / uy)
            elif uy < -1e-4:
                t = min(t, -sy / uy)
            dashed(sx, sy, sx + ux * t, sy + uy * t, "#ccc", 1, 6,
                   (KO_HEADER, KO_COG, KO_TEXT, KO_LOGO))

    # Blips: tracked flight cyan (1.6x); the two placed others green (outside
    # the radius) at 1.0x — like blipColor() on the device.
    for p in [feat] + d.get("blips", []):
        px = cx + p["dx"] * scale
        py = cy - p["dy"] * scale
        if not (0 <= px <= 319 and 0 <= py <= 239) or blip_blocked(px, py):
            continue
        if p is feat:
            plane_icon(px, py, p["hdg"], "#0ff", 1.6)
        else:
            col = "#f00" if p["dist"] <= RADIUS_MI else "#0f0"
            plane_icon(px, py, p["hdg"], col, 1.0)
    circle(cx, cy, 3, fill="#fff")   # you


def place_planes(d):
    """Compose the radar scene for the screenshot: real planes/track, but the
    tracked flight moved inside the ring (track shifted with it) and two other
    real flights pinned just outside the ring where the device's keep-out
    zones leave room."""
    feat = d["feat"]
    tx, ty = 1.7, -1.5            # inside the ring, lower-right quadrant
    ox, oy = tx - feat["dx"], ty - feat["dy"]
    feat["dx"], feat["dy"] = tx, ty
    feat["dist"] = math.hypot(tx, ty)
    d["track"] = [(x + ox, y + oy) for x, y in d["track"]]
    others = [p for p in d["planes"] if p is not feat][:2]
    for p, (dx, dy) in zip(others, [(4.4, 1.8), (-2.6, -3.9)]):
        p["dx"], p["dy"] = dx, dy   # east edge and bottom edge of the radar
        p["dist"] = math.hypot(dx, dy)
    d["blips"] = others


def draw_flight(d):
    feat = d["feat"] or d["planes"][0]
    place_planes(d)
    rect(0, 0, W, H, "#000", "#333", 1)
    header(d)
    cog()
    # Back button top-right, drawn over the header like drawFlightBackButton()
    rect(265, 4, 50, 20, BTN, rx=5)
    txt(290, 18, 11, "#fff", "Back", anchor="middle")
    txt(8, 62, 24, "#fff", feat["cs"] or feat["icao24"].upper())
    name = AIRLINES.get((feat["cs"] or "")[:3].upper(), "")
    if name:
        txt(8, 80, 11, "#0ff", name[:26])
    txt(8, 100, 11, "#ff0",
        f'{feat["altFt"]}ft  {feat["mph"]} mph  {feat["dist"]:.1f}mi')
    rt = d["route"]
    y = 120
    if rt:
        txt(8, y, 11, "#0ff", "Origin")
        txt(8, y + 15, 11, "#fff", rt["o"]["code"][:22])
        txt(8, y + 30, 10, "#fff", rt["o"]["city"][:26])
        y += 48
        txt(8, y, 11, "#0ff", "Destination")
        txt(8, y + 15, 11, "#fff", rt["d"]["code"][:22])
        txt(8, y + 30, 10, "#fff", rt["d"]["city"][:26])
    else:
        txt(8, y, 11, "#ccc", "No route data")
    radar(d)
    txt(8, 232, 9, "#ccc", f'{len(d["planes"])} aircraft')


def wx_icon(x, y, code, day=True):
    """Mirror of drawWeatherIcon(): the same numeric WMO ranges, not a per-code
    table. A table drifted here - a rain code drew no cloud (the cloud was only
    emitted for the two "cloudy" kinds) and snow/storm drew nothing at all."""
    rain  = 51 <= code <= 67 or 80 <= code <= 82
    snow  = 71 <= code <= 77 or 85 <= code <= 86
    storm = code >= 95
    cloud = code >= 1

    if day:
        circle(x + 12, y + 8, 5, "#ff0", 1, fill="#ff0")
        for a in range(0, 360, 45):
            r = math.radians(a)
            line(x + 12 + 6 * math.cos(r), y + 8 + 6 * math.sin(r),
                 x + 12 + 10 * math.cos(r), y + 8 + 10 * math.sin(r), "#ff0", 1.5)
    else:
        circle(x + 12, y + 8, 5, "#ccc", 1, fill="#ccc")
        circle(x + 15, y + 8, 5, "#000", 1, fill="#000")   # crescent cutout

    if cloud:
        circle(x + 8, y + 14, 4, "#777", 1, fill="#777")
        circle(x + 14, y + 11, 5, "#777", 1, fill="#777")
        circle(x + 19, y + 14, 4, "#777", 1, fill="#777")

    if rain:
        line(x + 8, y + 19, x + 7, y + 22, "#0ff", 1.5)
        line(x + 14, y + 19, x + 13, y + 22, "#0ff", 1.5)
        line(x + 20, y + 19, x + 19, y + 22, "#0ff", 1.5)
    elif snow:
        circle(x + 8, y + 21, 1, "#fff", 1, fill="#fff")
        circle(x + 14, y + 21, 1, "#fff", 1, fill="#fff")
        circle(x + 20, y + 21, 1, "#fff", 1, fill="#fff")
    elif storm:
        line(x + 12, y + 18, x + 8, y + 22, "#ff0", 1.5)


def hm12(v):
    """12h HH:MMA from either an HH:MM string or a unixtime epoch (local tz)."""
    import datetime
    try:
        if isinstance(v, int):
            t = datetime.datetime.fromtimestamp(v)
            return f"{t.hour % 12 or 12}:{t.minute:02d}{'A' if t.hour < 12 else 'P'}"
        h, m = int(v[:2]), int(v[3:5])
        return f"{h % 12 or 12}:{m:02d}{'A' if h < 12 else 'P'}"
    except Exception:
        return "--"


def wind_dir8(from_deg):
    return ["N", "NE", "E", "SE", "S", "SW", "W", "NW"][round(from_deg / 45.0) % 8]


# Wind tiers in knots, mirroring WIND_*_KT / GUST_*_KT in weather.ino.
def wind_col(kt, gust=False):
    yel, org, red = (17.0, 30.0, 40.0) if gust else (13.0, 22.0, 27.0)
    if kt >= red:
        return "#ff0000"
    if kt >= org:
        return "#fda600"
    if kt >= yel:
        return "#ffff00"
    return "#00ff00"


def wind_arrow(cx, cy, r, from_deg, col):
    """Mirror of drawWindArrow(): shaft + head pointing downwind, spanning 2r."""
    a = math.radians(from_deg + 180.0)
    dx, dy = math.sin(a), -math.cos(a)
    px, py = -dy, dx
    tipx, tipy = cx + r * dx, cy + r * dy
    hl, hw = r * 0.8, r * 0.45
    bx, by = tipx - hl * dx, tipy - hl * dy
    line(cx - r * dx, cy - r * dy, bx, by, col, 1.2)
    tri((tipx, tipy), (bx + hw * px, by + hw * py), (bx - hw * px, by - hw * py),
        col, stroke=None)


def draw_idle(d):
    rect(0, 0, W, H, "#000", "#333", 1)
    header(d)
    cog()
    cur = (d["wx"] or {}).get("current", {})
    daily = (d["wx"] or {}).get("daily", {})
    temp = cur.get("temperature_2m", 0)
    top = 42
    txt(8, top + 24, 26, "#fff", f"{round(temp)}F")
    wx_icon(78, top + 6, cur.get("weather_code", 0))
    txt(8, top + 41, 11, "#ccc",
        f'FL {round(cur.get("apparent_temperature", temp))}F')
    txt(8, top + 61, 11, "#0ff",
        f'{int(cur.get("relative_humidity_2m", 0))}% hum')
    sr = (daily.get("sunrise") or ["-"])[0]
    ss = (daily.get("sunset") or ["-"])[0]
    txt(8, top + 83, 11, "#ff0", "Sunr " + hm12(sr))
    txt(8, top + 101, 11, "#f80", "Suns " + hm12(ss))
    wkt, gkt = cur.get("wind_speed_10m"), cur.get("wind_gusts_10m")
    if wkt is not None:
        wdir = cur.get("wind_direction_10m") or 0
        wcol = wind_col(wkt)
        txt(8, top + 119, 11, wcol, "Wind")
        # No +11 baseline offset on the arrow: its cy is a centre, not a text top.
        wind_arrow(50, top + 116, 8, wdir, wcol)
        txt(8, top + 137, 11, wcol,
            f"{wind_dir8(wdir)} {round(wkt * 1.15078)} mph")
        txt(8, top + 155, 11, wind_col(gkt or 0, True),
            f"Gusts {round((gkt or 0) * 1.15078)} mph")
    else:
        txt(8, top + 119, 11, "#c0c0c0", "Wind")
        txt(8, top + 137, 11, "#c0c0c0", "--")
        txt(8, top + 155, 11, "#c0c0c0", "Gusts --")
    if d["pool"] is not None:
        txt(8, top + 173, 11, LAV, "Pool ")
        txt(50, top + 173, 11, LAV, f'{d["pool"]:.1f}F')
    x0, y0, w, h = 110, top + 2, 46, 92
    days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
    wd = (MOCK_DT.weekday() + 1) % 7  # Mon=0 -> Sun-first
    hi = daily.get("temperature_2m_max") or [0] * 8
    lo = daily.get("temperature_2m_min") or [0] * 8
    rn = daily.get("precipitation_probability_max") or [0] * 8
    wc = daily.get("weather_code") or [0] * 8
    for i in range(8):
        col, row = i % 4, i // 4
        x, y = x0 + col * w, y0 + row * h
        txt(x, y + 8, 9, "#fff", days[(wd + i) % 7])
        wx_icon(x + 4, y + 12, wc[i])
        txt(x, y + 48, 9, "#f9c", str(round(hi[i])))
        txt(x + 22, y + 48, 9, "#0ff", str(round(lo[i])))
        txt(x + 10, y + 62, 8, "#7fff00", f"{int(rn[i])}%")
    txt(8, 232, 9, "#ccc", f'{len(d["planes"])} aircraft')


def draw_wxgraph(d):
    """Weather Temperature History screen (Day view) — same layout as
    drawWxGraph(): theme header + Back, timeframe buttons, button-color plot
    box, pale theme-blend series, complementary dotted avg, corner scale
    labels, Lo/now/Hi footer."""
    rect(0, 0, W, H, "#000", "#333", 1)
    rect(0, 0, 320, 28, TH)
    txt(8, 19, 11, "#fff", "History > Weather Temperature")
    rect(265, 4, 50, 20, BTN, rx=5)
    txt(290, 18, 11, "#fff", "Back", anchor="middle")

    bx = 8
    for i, lbl in enumerate(("Day", "Week", "Month", "Year")):
        col = BTN if i == 0 else "#444"   # selected = button color
        rect(bx, 34, 70, 22, col, rx=5)
        txt(bx + 35, 49, 9, "#fff", lbl, anchor="middle")
        bx += 76

    gx, gy, gw, gh = 10, 66, 300, 140
    rect(gx, gy, gw, gh, BTN, stroke="#fff", sw=1)

    # Anchor the 24h window to the data's own "now" (open-meteo current.time
    # at fetch), not the wall clock: the series then ends at the same moment
    # the footer prints as "now", and cached renders stay identical however
    # old mock_data.json is.
    now = int(((d.get("wx") or {}).get("current") or {}).get("time") or
              max((t for t, _ in d["wx_hist"]), default=time.time()))
    win, t0 = 86400, now - 86400
    series = [(t, v) for t, v in d["wx_hist"] if t0 <= t <= now]
    if not series:
        txt(gx + 20, gy + gh // 2, 9, "#ccc", "No data in this period yet")
        return
    vals = [v for _, v in series]
    vmin, vmax = min(vals), max(vals)
    lo, hi = vmin, vmax
    if vmax - vmin < 1.0:
        vmin -= 1.0
        vmax += 1.0
    span = vmax - vmin

    prev = None
    for t, v in series:
        px = gx + (t - t0) * gw / win
        py = gy + gh - (v - vmin) * gh / span
        px = min(max(px, gx), gx + gw)
        py = min(max(py, gy), gy + gh)
        if prev:
            line(prev[0], prev[1], px, py, GLN, 1)
        prev = (px, py)

    avg = sum(vals) / len(vals)
    avgy = min(max(gy + gh - (avg - vmin) * gh / span, gy), gy + gh)
    x = gx
    while x <= gx + gw:
        line(x, avgy, min(x + 3, gx + gw), avgy, GAVG, 1)
        x += 6
    txt(gx + 2, avgy - 3, 8, GAVG, f"avg {avg:.1f}")
    txt(gx + gw - 2, gy + 10, 8, "#fff", f"{vmax:.0f}", anchor="end")
    txt(gx + gw - 2, gy + gh - 2, 8, "#fff", f"{vmin:.0f}", anchor="end")

    cur = (d["wx"] or {}).get("current", {})
    txt(gx, gy + gh + 18, 11, "#0ff", f"Lo {lo:.1f}")
    txt(gx + 110, gy + gh + 18, 11, "#fff",
        f'now {cur.get("temperature_2m", 0):.1f}F')
    txt(gx + gw, gy + gh + 18, 11, "#0ff", f"Hi {hi:.1f}", anchor="end")


GY = "#adff2e"   # TFT_GREENYELLOW


def toggle_row(y, label, value, on=True):
    """label (font2 white) + value + theme Toggle pill — the shared row
    pattern used by the settings screens."""
    txt(8, y + 13, 11, "#fff", label)
    txt(150, y + 13, 11, GY if on else "#ccc", value)
    rect(230, y - 4, 82, 24, BTN, rx=5)
    txt(271, y + 11, 9, "#fff", "Toggle", anchor="middle")


def slider_row(y, label, value):
    """drawSlider(): label + value + [▼][▲] stepper (buttons at x=246; the
    value follows the label so unit suffixes fit)."""
    txt(8, y + 13, 11, "#fff", label)
    vx = 8 + len(label) * 12 + 14   # approx tft.textWidth(label, font2)
    txt(vx, y + 17, 11, GY, value)
    adj_pair(246, y)


def draw_ftracker(d):
    """Flight Tracker settings, page 1/3 — Enabled toggle and the
    Radius/Ceiling/Poll steppers, with the device's defaults. (Units moved
    to the General page.)"""
    rect(0, 0, W, H, "#000", "#333", 1)
    rect(0, 0, 320, 28, TH)
    txt(8, 19, 11, "#fff", "Settings > Flight Tracker")
    rect(265, 4, 50, 20, BTN, rx=5)
    txt(290, 18, 11, "#fff", "Back", anchor="middle")

    toggle_row(40, "Enabled", "ON")

    slider_row(76, "Radius (mi)", "3.5")
    txt(8, 106, 8, "#ccc", "how far away to look")
    slider_row(124, "Ceiling (ft)", "15000")
    txt(8, 154, 8, "#ccc", "ignore planes above this")
    slider_row(172, "Poll (s)", "30")
    txt(8, 202, 8, "#ccc", "how often to check for planes")

    txt(160, 230, 9, "#ccc", "Page 1/3", anchor="middle")
    rect(264, 214, 44, 26, BTN, rx=6)
    txt(286, 233, 11, "#fff", ">", anchor="middle")


def adj_pair(x, y):
    """adjPair(): horizontal [▼][▲] stepper buttons in the theme color —
    down (decrement) left, up (increment) right (30px each, 24px tall).
    Triangles are ~text-sized (8x6), not button-filling."""
    rect(x, y, 30, 24, BTN, rx=5)
    tri((x + 15, y + 15), (x + 11, y + 9), (x + 19, y + 9), "#fff", "#fff")
    rect(x + 34, y, 30, 24, BTN, rx=5)
    tri((x + 34 + 15, y + 9), (x + 34 + 11, y + 15), (x + 34 + 19, y + 15),
        "#fff", "#fff")


def draw_alarms(d):
    """Alarms > 2 editor — Enabled toggle, hour [▼▲]/time/minute [▼▲] row,
    weekday toggles, Notify preset stepper, and the < | Del | New footer
    (alarm 2 of 2 so all three buttons show)."""
    rect(0, 0, W, H, "#000", "#333", 1)
    rect(0, 0, 320, 28, TH)
    txt(8, 19, 11, "#fff", "Alarms > 2")
    rect(265, 4, 50, 20, BTN, rx=5)
    txt(290, 18, 11, "#fff", "Back", anchor="middle")

    txt(8, 57, 11, "#fff", "Enabled")
    txt(150, 57, 11, GY, "ON")
    rect(230, 40, 82, 24, BTN, rx=5)
    txt(271, 55, 9, "#fff", "Toggle", anchor="middle")

    # drawTimeAdj row: hour [▼▲] left, big time, stacked AM/PM indicator
    # (active bright on top/bottom like the header), minute [▼▲] right
    txt(8, 93, 11, "#fff", "Time")
    adj_pair(80, 80)
    txt(190, 104, 22, GY, "7:30", anchor="middle")
    txt(230, 89, 9, GY, "AM")
    txt(230, 105, 9, "#444", "PM")
    adj_pair(252, 80)

    # Weekday toggles (Mon-Fri selected for a workday alarm)
    txt(8, 146, 11, "#fff", "Days")
    for i, dch in enumerate("SMTWTFS"):
        sel = i in (1, 2, 3, 4, 5)
        bx = 64 + i * 36
        rect(bx, 128, 32, 26, BTN if sel else "#444", rx=4)
        txt(bx + 16, 146, 11, "#fff", dch, anchor="middle")

    txt(8, 185, 11, "#fff", "Notify")
    txt(200, 185, 11, GY, "Blink", anchor="middle")
    adj_pair(252, 166)

    # Footer: < prev | Delete (center) | New (right, on the last alarm)
    rect(8, 206, 44, 26, BTN, rx=5)
    txt(30, 224, 11, "#fff", "&lt;", anchor="middle")
    rect(120, 206, 80, 26, DGR, rx=5)
    txt(160, 224, 11, "#fff", "Delete", anchor="middle")
    rect(228, 206, 84, 26, BTN, rx=5)
    txt(270, 224, 11, "#fff", "New", anchor="middle")


def emit_png(path):
    src = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d"'
           ' viewBox="0 0 %d %d">%s</svg>' % (W, H, W, H, "".join(svg)))
    import cairosvg
    cairosvg.svg2png(bytestring=src.encode(), write_to=str(path),
                     output_width=W * 3, output_height=H * 3)
    svg.clear()


def main():
    monitor = 90
    if "--monitor" in sys.argv:
        monitor = int(sys.argv[sys.argv.index("--monitor") + 1])
    elif "--once" in sys.argv:
        monitor = 0

    # Static data cache: render from mock_data.json unless --refresh (or the
    # cache doesn't exist yet), so PDF regeneration doesn't hit the APIs.
    if "--refresh" not in sys.argv and OUT_DATA.exists():
        d = json.loads(OUT_DATA.read_text())
        print("data: cached mock_data.json (use --refresh to re-pull)")
    else:
        d = fetch_all(load_env(), monitor)
        try:
            OUT_DATA.write_text(json.dumps(d))
            print("data: live fetch -> mock_data.json")
        except Exception:
            print("data: live fetch (cache save failed)")

    if d["feat"]:
        draw_flight(d)
        view = ("flight-overhead (%s, %d track pts)"
                % (d["feat"]["cs"] or d["feat"]["icao24"], len(d["track"])))
    else:
        draw_idle(d)
        view = "idle"
    emit_png(OUT_DASH)
    draw_idle(d)
    emit_png(OUT_IDLE)
    draw_wxgraph(d)
    emit_png(OUT_GRAPH)
    draw_ftracker(d)
    emit_png(OUT_FTRK)
    draw_alarms(d)
    emit_png(OUT_ALRM)

    print(f"wrote {OUT_DASH.name} ({view})")
    print(f"wrote {OUT_IDLE.name} (pool={d['pool']})")
    print(f"wrote {OUT_GRAPH.name} ({len(d['wx_hist'])} hourly pts)")
    print(f"wrote {OUT_FTRK.name}")
    print(f"wrote {OUT_ALRM.name}")
    print(f"  planes={len(d['planes'])} crp={d['crp']} crl={d['crl']} "
          f"cft={d['cft']}")


if __name__ == "__main__":
    main()
