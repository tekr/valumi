#!/usr/bin/env python3
"""
Integration test for the valumi web panel, driven over the LAN
against a real device.

Python 3 standard library only (http.client, json, gzip, argparse) --
`requests` is deliberately not used.

Usage:
    python3 test_panel.py --host valumi.local --password <panel password>
    python3 test_panel.py --host 192.168.1.42 --password secret123 -v
    python3 test_panel.py --host valumi.local --password secret123 \\
        --firmware ../../build/valumi.bin

See README.md in this directory for the full story, safety notes, and
what each test covers.

Exit status is non-zero if any test failed.
"""

import argparse
import gzip
import http.client
import json
import re
import secrets
import sys
import time
import traceback

# Keep in step with settings.h / auth_core.h on the device -- these are the
# server's own limits, not arbitrary choices here.
SET_MAX_COINS = 15          # settings.h: SET_MAX_COINS
AUTH_MAX_FAILS = 5         # auth_core.h: AUTH_MAX_FAILS
AUTH_LOCKOUT_S = 30        # auth_core.h: AUTH_LOCKOUT_US

# The "live" settings fields the panel can change without a restart. This is
# exactly what gets snapshotted before the run and restored afterwards --
# "networks" is deliberately not in this list (see the module docstring /
# README: we never touch Wi-Fi). There is no device name any more.
LIVE_FIELDS = ("coins", "dwell_s", "orientation", "brightness", "transition_fade",
              "transition_style", "transition_ms", "transition", "transition_direction", "range", "chart_style", "night",
              "tz_offset_min")

DEFAULT_TIMEOUT = 12.0

SID_RE = re.compile(r"sid=([0-9a-f]{32})")


# ---------------------------------------------------------------------------
# Tiny HTTP client (http.client, not requests/urllib -- we want full control
# over redirects, cookies, and Content-Type, none of which urllib's default
# opener leaves alone).
# ---------------------------------------------------------------------------

class Resp:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers  # list of (name, value), as received
        self.body = body

    def header(self, name):
        name = name.lower()
        for k, v in self.headers:
            if k.lower() == name:
                return v
        return None

    def json(self):
        return json.loads(self.body.decode("utf-8"))

    def text(self):
        return self.body.decode("utf-8", "replace")


class Client:
    def __init__(self, host, port=80, timeout=DEFAULT_TIMEOUT, verbose=False):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.verbose = verbose

    def request(self, method, path, body=None, headers=None, sid=None, timeout=None):
        hdrs = dict(headers or {})
        if sid:
            hdrs["Cookie"] = "sid=%s" % sid
        # One request per connection: the device has only a handful of
        # sockets (see web_panel.c's max_open_sockets), and a long test run
        # has no business holding one open between requests.
        hdrs.setdefault("Connection", "close")
        data = body
        if isinstance(data, str):
            data = data.encode("utf-8")

        if self.verbose:
            print("    > %s %s%s" % (method, path,
                                     " (%d byte body)" % len(data) if data else ""))
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout or self.timeout)
        try:
            try:
                conn.request(method, path, body=data, headers=hdrs)
            except (BrokenPipeError, ConnectionResetError):
                # The device may have already answered (e.g. 413) and closed
                # its read side before we finished sending; the response is
                # still worth reading.
                pass
            resp = conn.getresponse()
            raw = resp.read()
            r = Resp(resp.status, resp.reason, resp.getheaders(), raw)
            if self.verbose:
                print("    < %d %s" % (r.status, r.reason))
            return r
        finally:
            conn.close()


class Ctx:
    def __init__(self, client, args):
        self.client = client
        self.args = args
        self.sid = None          # current session cookie value, once logged in
        self.snapshot = None     # /api/settings at the very start, for restore
        self.ota_before_partition = None


_USE_CTX_SID = object()


def get(ctx, path, timeout=None, sid=_USE_CTX_SID):
    s = ctx.sid if sid is _USE_CTX_SID else sid
    return ctx.client.request("GET", path, sid=s, timeout=timeout)


def put_json(ctx, path, obj, timeout=None, sid=_USE_CTX_SID):
    s = ctx.sid if sid is _USE_CTX_SID else sid
    return ctx.client.request("PUT", path, body=json.dumps(obj),
                              headers={"Content-Type": "application/json"}, sid=s,
                              timeout=timeout)


def post_json(ctx, path, obj, timeout=None, sid=_USE_CTX_SID):
    s = ctx.sid if sid is _USE_CTX_SID else sid
    return ctx.client.request("POST", path, body=json.dumps(obj),
                              headers={"Content-Type": "application/json"}, sid=s,
                              timeout=timeout)


def assert_status(r, expected):
    if r.status != expected:
        raise AssertionError("expected HTTP %d, got %d %s: %s" %
                             (expected, r.status, r.reason, r.text()[:300]))


def refused(ctx, method, path, body, headers, sid, expect_status, timeout=10):
    """A request expected to be refused before its body is fully read (401,
    413, 415). The server closes the connection in that case rather than
    reading and answering, so either the expected status or the connection
    being reset/closed counts as a pass -- it must never hang or raise."""
    try:
        r = ctx.client.request(method, path, body=body, headers=headers, sid=sid,
                               timeout=timeout)
    except (http.client.HTTPException, ConnectionResetError, BrokenPipeError, OSError):
        return
    assert r.status == expect_status, (
        "expected HTTP %d (or a closed connection), got %d %s: %s" %
        (expect_status, r.status, r.reason, r.text()[:300]))


# ---------------------------------------------------------------------------
# Tests: page and unauthenticated basics
# ---------------------------------------------------------------------------

def test_index_page_gzip(ctx):
    r = ctx.client.request("GET", "/")
    assert_status(r, 200)
    enc = r.header("Content-Encoding")
    assert enc == "gzip", "expected Content-Encoding: gzip, got %r" % enc
    html = gzip.decompress(r.body).decode("utf-8", "replace")
    assert "<html" in html.lower(), "decompressed page has no <html"


def test_state_unauth(ctx):
    r = ctx.client.request("GET", "/api/state")
    assert_status(r, 200)
    j = r.json()
    assert j.get("mode") == "online", (
        "expected mode 'online', got %r -- is the ticker on the network and "
        "connected?" % j.get("mode"))
    assert j.get("hostname") == "valumi", "expected hostname 'valumi', got %r" % j.get("hostname")
    assert j.get("version"), "expected a non-empty version"
    assert isinstance(j.get("logged_in"), bool), "expected a boolean logged_in"
    assert "name" not in j, "there is no device name any more: %r" % j


def test_login_case_insensitive(ctx):
    # The panel password is compared case-insensitively.
    r = post_json(ctx, "/api/login", {"password": ctx.args.password.lower()}, sid=None)
    assert_status(r, 200)
    m = SID_RE.search(r.header("Set-Cookie") or "")
    assert m, "no session cookie after a lower-case password login"
    post_json(ctx, "/api/logout", {}, sid=m.group(1))


UNAUTH_ENDPOINTS = [
    ("GET", "/api/settings"),
    ("PUT", "/api/settings"),
    ("GET", "/api/status"),
    ("GET", "/api/scan"),
    ("GET", "/api/export"),
    ("POST", "/api/import"),
    ("POST", "/api/wifi"),
    ("POST", "/api/ota"),
    ("POST", "/api/restart"),
]


def test_unauthenticated_401(ctx):
    # authorised() runs before any content-type or body check in every one of
    # these handlers, so a bare request with no cookie and no body must be
    # refused before anything it carries is looked at.
    bad = []
    for method, path in UNAUTH_ENDPOINTS:
        r = ctx.client.request(method, path, sid=None, timeout=8)
        if r.status != 401:
            bad.append("%s %s -> %d (want 401)" % (method, path, r.status))
    assert not bad, "; ".join(bad)


def test_login_wrong_password(ctx):
    r = post_json(ctx, "/api/login", {"password": ctx.args.password + "-definitely-wrong"},
                 sid=None)
    assert_status(r, 401)


def test_qr_login_bogus_token(ctx):
    bogus = secrets.token_hex(16)  # 32 hex chars, astronomically unlikely to be live
    r = ctx.client.request("GET", "/login?t=%s" % bogus)
    assert r.status == 302, "expected a 302 redirect, got %d" % r.status
    loc = r.header("Location")
    assert loc and "qr=expired" in loc, "expected redirect to /?qr=expired, got %r" % loc
    assert r.header("Set-Cookie") is None, "a spent/bogus QR token must not set a cookie"


def test_garbage_cookie_rejected(ctx):
    for bad_sid in ("deadbeefdeadbeefdeadbeefdeadbeef", "not-hex-at-all-garbage-cookie"):
        r = get(ctx, "/api/settings", sid=bad_sid)
        assert_status(r, 401)


def test_login_correct(ctx):
    r = post_json(ctx, "/api/login", {"password": ctx.args.password}, sid=None)
    assert_status(r, 200)
    sc = r.header("Set-Cookie")
    assert sc, "no Set-Cookie header on a successful login"
    m = SID_RE.search(sc)
    assert m, "Set-Cookie has no sid value: %r" % sc
    assert "HttpOnly" in sc, "Set-Cookie missing HttpOnly: %r" % sc
    assert "SameSite=Strict" in sc, "Set-Cookie missing SameSite=Strict: %r" % sc
    ctx.sid = m.group(1)


# ---------------------------------------------------------------------------
# Tests: settings (authenticated)
# ---------------------------------------------------------------------------

def test_put_wrong_content_type(ctx):
    r = ctx.client.request("PUT", "/api/settings", body=b"{}",
                           headers={"Content-Type": "text/plain"}, sid=ctx.sid)
    assert_status(r, 415)


def test_settings_no_password_leak(ctx):
    r = get(ctx, "/api/settings")
    assert_status(r, 200)
    text = r.text()
    assert ctx.args.password not in text, "the panel password appears in /api/settings!"
    j = r.json()
    for net in j.get("networks", []):
        assert "password" not in net, "network entry leaks a password field: %r" % net
        assert "has_password" in net, "network entry missing has_password: %r" % net


def test_put_unknown_key(ctx):
    r = put_json(ctx, "/api/settings", {"totally_bogus_field": 1})
    assert_status(r, 400)
    err = r.json().get("error", "")
    assert "totally_bogus_field" in err, "error should name the bad key: %r" % err


def test_put_name_rejected(ctx):
    # There is no device name any more; "name" is just another unknown key.
    r = put_json(ctx, "/api/settings", {"name": "new-name"})
    assert_status(r, 400)
    err = r.json().get("error", "")
    assert "name" in err, "error should name the rejected 'name' key: %r" % err


def test_put_dwell_bounds(ctx):
    for bad in (-1, 16):
        r = put_json(ctx, "/api/settings", {"dwell_s": bad})
        assert_status(r, 400)


def test_put_brightness_bounds(ctx):
    r = put_json(ctx, "/api/settings", {"brightness": 500})
    assert_status(r, 400)


def test_put_atomicity(ctx):
    before = get(ctx, "/api/settings").json()
    r = put_json(ctx, "/api/settings", {"dwell_s": 12, "brightness": 500})
    assert_status(r, 400)
    after = get(ctx, "/api/settings").json()
    assert after["dwell_s"] == before["dwell_s"], (
        "dwell_s changed even though the PUT was rejected as a whole "
        "(%r -> %r)" % (before["dwell_s"], after["dwell_s"]))


def test_put_live_roundtrip(ctx):
    cur = get(ctx, "/api/settings").json()

    new_dwell = 12 if cur["dwell_s"] != 12 else 10
    new_bright = 77 if cur["brightness"] != 77 else 55
    new_range = (cur["range"] + 1) % 3
    styles = ["close", "highs_lows", "band"]
    new_chart = styles[(styles.index(cur["chart_style"]) + 1) % len(styles)]
    new_fixed = not cur["orientation"]["fixed"]
    new_usb_left = not cur["orientation"]["usb_left"]
    new_night_on = not cur["night"]["on"]
    new_start = "01:23" if cur["night"]["start"] != "01:23" else "02:34"
    new_end = "05:10" if cur["night"]["end"] != "05:10" and new_start != "05:10" else "07:40"
    new_night_bright = 66 if cur["night"]["brightness"] != 66 else 44
    tz = cur["tz_offset_min"]
    new_tz = tz + 15 if tz + 15 <= 840 else tz - 15

    payload = {
        "dwell_s": new_dwell,
        "brightness": new_bright,
        "range": new_range,
        "chart_style": new_chart,
        "orientation": {"fixed": new_fixed, "usb_left": new_usb_left},
        "night": {"on": new_night_on, "start": new_start, "end": new_end,
                 "brightness": new_night_bright},
        "tz_offset_min": new_tz,
    }
    r = put_json(ctx, "/api/settings", payload)
    assert_status(r, 200)
    body = r.json()
    assert body["ok"] is True
    assert body["restart_needed"] is False, "a live-field change should never need a restart"

    def check(s, where):
        assert s["dwell_s"] == new_dwell, where
        assert s["brightness"] == new_bright, where
        assert s["range"] == new_range, where
        assert s["chart_style"] == new_chart, where
        assert s["orientation"]["fixed"] == new_fixed, where
        assert s["orientation"]["usb_left"] == new_usb_left, where
        assert s["night"]["on"] == new_night_on, where
        assert s["night"]["start"] == new_start, where
        assert s["night"]["end"] == new_end, where
        assert s["night"]["brightness"] == new_night_bright, where
        assert s["tz_offset_min"] == new_tz, where

    check(body["settings"], "in the PUT response")
    check(get(ctx, "/api/settings").json(), "in a following GET")


# ---------------------------------------------------------------------------
# Tests: coins
# ---------------------------------------------------------------------------

def test_put_coins_bad_id(ctx):
    r = put_json(ctx, "/api/settings", {"coins": [{"inst_id": "bad id!"}]})
    assert_status(r, 400)


def test_put_coins_zero(ctx):
    r = put_json(ctx, "/api/settings", {"coins": []})
    assert_status(r, 400)


def test_put_coins_too_many(ctx):
    coins = [{"inst_id": "A%d-USDT" % i} for i in range(16)]
    r = put_json(ctx, "/api/settings", {"coins": coins})
    assert_status(r, 400)


def test_put_coins_duplicate(ctx):
    r = put_json(ctx, "/api/settings",
                {"coins": [{"inst_id": "BTC-USDT"}, {"inst_id": "BTC-USDT"}]})
    assert_status(r, 400)
    err = r.json().get("error", "")
    assert "twice" in err.lower(), "expected a 'listed twice' style error, got %r" % err


def test_put_coins_unknown_exchange(ctx):
    r = put_json(ctx, "/api/settings", {"coins": [{"inst_id": "ZZZNOPECOIN-USDT"}]}, timeout=35)
    assert_status(r, 400)
    err = r.json().get("error", "")
    assert "does not list" in err, "expected a 'does not list' error, got %r" % err


def test_put_coins_add_valid(ctx):
    snap_coins = ctx.snapshot["coins"]
    have = {c["inst_id"] for c in snap_coins}
    candidate = None
    # Enough candidates that one is free even on a full ticker.
    for cand in ("DOGE-USDT", "XRP-USDT", "ADA-USDT", "LTC-USDT", "OKB-USDT", "DOT-USDT",
                 "ETC-USDT", "UNI-USDT", "NEAR-USDT", "APT-USDT", "ATOM-USDT", "FIL-USDT",
                 "AAVE-USDT", "ARB-USDT", "OP-USDT", "INJ-USDT"):
        if cand not in have:
            candidate = cand
            break
    assert candidate, "could not find a spare coin symbol not already on the ticker"

    base = list(snap_coins)
    if len(base) >= SET_MAX_COINS:
        base = base[:SET_MAX_COINS - 1]
    lower_label = candidate.split("-")[0][:4].lower()
    new_list = base + [{"inst_id": candidate, "label": lower_label}]

    r = put_json(ctx, "/api/settings", {"coins": new_list}, timeout=35)
    assert_status(r, 200)
    j = r.json()
    assert j["ok"] is True
    got = {c["inst_id"]: c["label"] for c in j["settings"]["coins"]}
    assert candidate in got, "new coin missing from the response: %r" % got
    assert got[candidate] == lower_label.upper(), (
        "label was not upper-cased: sent %r, got %r" % (lower_label, got[candidate]))


# ---------------------------------------------------------------------------
# Tests: status, scan, export/import, oversize body
# ---------------------------------------------------------------------------

def test_status_shape(ctx):
    # /api/status is just wifi{mode,ssid,ip,rssi}, system{version,
    # heap_min_free,partition}, clock_synced -- no board, uptime, heap_free,
    # heap_largest, hotspot_clients, coins, page, or stack_unused any more.
    r = get(ctx, "/api/status", timeout=15)
    assert_status(r, 200)
    j = r.json()
    assert set(j.keys()) == {"wifi", "system", "clock_synced"}, (
        "unexpected top-level keys: %r" % sorted(j.keys()))
    assert j["wifi"]["mode"] == "online", j["wifi"]
    assert j["wifi"]["ip"], "expected a non-empty IP address"
    assert isinstance(j["wifi"]["rssi"], int), "expected an integer rssi"
    assert set(j["system"].keys()) == {"version", "heap_min_free", "partition"}, (
        "unexpected system keys: %r" % sorted(j["system"].keys()))
    assert j["system"]["version"], "expected a non-empty firmware version"
    assert isinstance(j["system"]["heap_min_free"], int), "expected an integer heap_min_free"
    assert j["system"]["partition"], "expected a non-empty partition label"

    deadline = time.time() + 30
    synced = j.get("clock_synced")
    while time.time() < deadline and not synced:
        time.sleep(3)
        synced = get(ctx, "/api/status", timeout=15).json().get("clock_synced")
    assert synced, "clock did not sync within 30s"


def test_scan(ctx):
    r = get(ctx, "/api/scan", timeout=30)
    if r.status == 503:
        time.sleep(3)
        r = get(ctx, "/api/scan", timeout=30)
    assert_status(r, 200)
    assert isinstance(r.json().get("networks"), list)


def test_export_import_roundtrip(ctx):
    pre = get(ctx, "/api/settings", timeout=10).json()

    rexp = get(ctx, "/api/export", timeout=10)
    assert_status(rexp, 200)
    exported = rexp.json()
    assert "name" not in exported, "there is no device name to export: %r" % exported
    assert "panel_password" not in exported, "the panel password belongs to the ticker"
    for net in exported.get("networks", []):
        assert "password" in net, "a backup needs each network's password: %r" % net

    rimp = post_json(ctx, "/api/import", exported, timeout=15)
    assert_status(rimp, 200)
    j = rimp.json()
    assert j["ok"] is True
    assert j["restart_needed"] is False, "reimporting unchanged settings should not restart"
    assert "settings" in j, "import response should include the resulting settings: %r" % j

    post = get(ctx, "/api/settings", timeout=10).json()
    for key in LIVE_FIELDS:
        assert post.get(key) == pre.get(key), (
            "settings changed after an export/import round trip: %s (%r != %r)" %
            (key, post.get(key), pre.get(key)))


def test_oversize_body(ctx):
    body = b'{"padding":"' + b"x" * 4200 + b'"}'
    refused(ctx, "PUT", "/api/settings", body, {"Content-Type": "application/json"}, ctx.sid,
           413, timeout=15)


def test_login_oversize_body(ctx):
    # Login bodies are held to a much tighter limit (256 bytes) than other
    # endpoints: it is the one request anyone on the network can send without
    # a session at all.
    body = json.dumps({"password": "x" * 300}).encode("utf-8")
    refused(ctx, "POST", "/api/login", body, {"Content-Type": "application/json"}, None,
           413, timeout=15)


def test_wide_flat_array_rejected(ctx):
    # ~1000 top-level elements, comfortably under the 4 KB body limit, but
    # well past the 256-element budget (counted from '[ { , :' outside
    # strings) that bounds what cJSON is asked to allocate.
    body = "[" + ",".join(["0"] * 1000) + "]"
    assert len(body) < 4096, "test body grew past the byte limit: %d" % len(body)
    r = ctx.client.request("PUT", "/api/settings", body=body,
                           headers={"Content-Type": "application/json"}, sid=ctx.sid,
                           timeout=10)
    assert_status(r, 400)


# ---------------------------------------------------------------------------
# Test: lockout
# ---------------------------------------------------------------------------

def test_lockout(ctx):
    got_429 = False
    for _ in range(AUTH_MAX_FAILS + 1):
        r = post_json(ctx, "/api/login",
                      {"password": "wrong-" + secrets.token_hex(3)}, sid=None, timeout=10)
        if r.status == 429:
            got_429 = True
            break
        if r.status != 401:
            raise AssertionError("expected 401 for a wrong password, got %d" % r.status)
    assert got_429, "did not get locked out after %d wrong passwords" % AUTH_MAX_FAILS

    r = post_json(ctx, "/api/login", {"password": ctx.args.password}, sid=None, timeout=10)
    assert_status(r, 429)

    time.sleep(AUTH_LOCKOUT_S + 1)

    r = post_json(ctx, "/api/login", {"password": ctx.args.password}, sid=None, timeout=10)
    assert_status(r, 200)
    m = SID_RE.search(r.header("Set-Cookie") or "")
    assert m, "no session cookie after the post-lockout login"
    post_json(ctx, "/api/logout", {}, sid=m.group(1))


# ---------------------------------------------------------------------------
# Optional tests: OTA (only run with --firmware)
# ---------------------------------------------------------------------------

def test_ota_reject_garbage(ctx):
    junk = b"\x00" * 1024
    r = ctx.client.request("POST", "/api/ota", body=junk,
                           headers={"Content-Type": "application/octet-stream"}, sid=ctx.sid,
                           timeout=20)
    assert_status(r, 400)
    assert r.json().get("error"), "expected an error message explaining the rejection"

    r2 = ctx.client.request("GET", "/api/state", timeout=10)
    assert_status(r2, 200)


def test_ota_upgrade(ctx):
    with open(ctx.args.firmware, "rb") as f:
        fw = f.read()

    r = ctx.client.request("POST", "/api/ota", body=fw,
                           headers={"Content-Type": "application/octet-stream"}, sid=ctx.sid,
                           timeout=120)
    assert_status(r, 200)

    time.sleep(3)  # restart_soon() fires ~1.5s after the response
    deadline = time.time() + 90
    up = False
    while time.time() < deadline and not up:
        try:
            if ctx.client.request("GET", "/api/state", timeout=5).status == 200:
                up = True
                break
        except Exception:
            pass
        time.sleep(3)
    assert up, "device did not come back within 90s of the OTA restart"

    rlogin = ctx.client.request("POST", "/api/login",
                                body=json.dumps({"password": ctx.args.password}),
                                headers={"Content-Type": "application/json"}, timeout=10)
    assert_status(rlogin, 200)
    m = SID_RE.search(rlogin.header("Set-Cookie") or "")
    assert m, "could not log back in after the OTA reboot"
    ctx.sid = m.group(1)

    rstatus = get(ctx, "/api/status", timeout=10)
    assert_status(rstatus, 200)
    new_partition = rstatus.json()["system"]["partition"]
    assert new_partition != ctx.ota_before_partition, (
        "partition did not change after OTA (still %r)" % new_partition)

    # The new image is on probation until it has been reachable for 60 s; a
    # restart before then rolls it back. Wait that out, restart, and check it
    # stayed -- the proof that it confirmed itself.
    time.sleep(70)
    assert_status(post_json(ctx, "/api/restart", {}), 200)
    time.sleep(5)
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            if ctx.client.request("GET", "/api/state", timeout=5).status == 200:
                break
        except Exception:
            pass
        time.sleep(3)
    rlogin = ctx.client.request("POST", "/api/login",
                                body=json.dumps({"password": ctx.args.password}),
                                headers={"Content-Type": "application/json"}, timeout=10)
    assert_status(rlogin, 200)
    ctx.sid = SID_RE.search(rlogin.header("Set-Cookie") or "").group(1)
    after = get(ctx, "/api/status", timeout=10).json()["system"]["partition"]
    assert after == new_partition, (
        "rolled back to %r after a restart: the new image never confirmed itself" % after)



# ---------------------------------------------------------------------------
# Tests: limits, restart, and the Wi-Fi request
# ---------------------------------------------------------------------------

def test_settings_limits_and_sensor(ctx):
    r = get(ctx, "/api/settings")
    assert_status(r, 200)
    s = r.json()
    lim = s.get("limits")
    assert lim, "no limits object"
    want = {
        "max_coins": 15, "max_nets": 4, "dwell_min": 0, "dwell_max": 15,
        "bright_min": 10, "bright_max": 100, "fade_max": 100,
        "transition_min_ms": 200, "transition_max_ms": 3000, "label_max": 4,
        "inst_max": 23, "ssid_max": 32, "wifi_pw_min": 8, "wifi_pw_max": 63,
    }
    for k, v in want.items():
        assert lim.get(k) == v, "limits.%s = %r, want %r" % (k, lim.get(k), v)
    assert "name_max" not in lim, "name_max should be gone with the device name: %r" % lim
    assert s.get("range_names") == ["1D", "7D", "30D"], (
        "range_names = %r" % s.get("range_names"))
    assert isinstance(s.get("orientation_sensor"), bool), "orientation_sensor must be a bool"


def test_json_nesting_limit(ctx):
    # Ten levels: past the panel's limit of 8, far short of anything harmful.
    body = '{"a":' + "[" * 10 + "]" * 10 + "}"
    r = ctx.client.request("PUT", "/api/settings", body=body,
                           headers={"Content-Type": "application/json"}, sid=ctx.sid)
    assert_status(r, 400)


def test_wifi_validation_rejects_before_saving(ctx):
    # Each of these is refused before anything is saved or restarted.
    r = post_json(ctx, "/api/wifi", {"networks": []})
    assert_status(r, 400)
    r = post_json(ctx, "/api/wifi", {"networks": [{
        "ssid": "x", "password": "", "static_ip": True, "ip": "192.168.1.300",
        "mask": "255.255.255.0", "gateway": "192.168.1.1"}]})
    assert_status(r, 400)
    # Only networks: the ticker makes its own panel password, so none is
    # accepted here any more -- and nothing else either.
    keep = [{"ssid": n["ssid"], "keep_password": True} if n["has_password"]
            else {"ssid": n["ssid"], "password": ""}
            for n in get(ctx, "/api/settings").json()["networks"]]
    r = post_json(ctx, "/api/wifi", {"networks": keep, "panel_password": "newpass1"})
    assert_status(r, 400)
    assert "unknown field" in r.json().get("error", ""), r.text()


def test_restart_needs_json(ctx):
    r = ctx.client.request("POST", "/api/restart", body="x",
                           headers={"Content-Type": "text/plain"}, sid=ctx.sid)
    assert_status(r, 415)
    time.sleep(3)
    assert_status(ctx.client.request("GET", "/api/state", timeout=5), 200)


def test_import_skips_unknown_network(ctx):
    exported = get(ctx, "/api/export").json()
    before = get(ctx, "/api/settings").json()["networks"]
    exported["networks"] = exported["networks"] + [
        {"ssid": "NoSuchNetwork-test", "has_password": True, "static_ip": False}]
    if len(exported["networks"]) > 4:
        exported["networks"] = exported["networks"][-4:]
    r = post_json(ctx, "/api/import", exported)
    assert_status(r, 200)
    body = r.json()
    assert any("NoSuchNetwork-test" in w for w in body.get("warnings", [])), r.text()
    assert "settings" in body, "import response should include the resulting settings: %r" % body
    after = get(ctx, "/api/settings").json()["networks"]
    assert [n["ssid"] for n in after] == [n["ssid"] for n in before], (
        "networks changed: %r -> %r" % (before, after))

# ---------------------------------------------------------------------------
# Test: logout (always last)
# ---------------------------------------------------------------------------

def test_logout(ctx):
    sid = ctx.sid
    r = post_json(ctx, "/api/logout", {}, sid=sid)
    assert_status(r, 200)
    r2 = get(ctx, "/api/settings", sid=sid)
    assert_status(r2, 401)


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

class Runner:
    def __init__(self, verbose=False):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.verbose = verbose

    def run(self, ctx, name, fn):
        start = time.time()
        try:
            fn(ctx)
        except AssertionError as e:
            self.failed += 1
            print("FAIL  %-58s %s" % (name, e))
        except Exception as e:
            self.failed += 1
            print("FAIL  %-58s %s: %s" % (name, type(e).__name__, e))
            if self.verbose:
                traceback.print_exc()
        else:
            self.passed += 1
            print("PASS  %-58s (%.1fs)" % (name, time.time() - start))

    def skip(self, name, reason):
        self.skipped += 1
        print("SKIP  %-58s %s" % (name, reason))

    def summary(self):
        total = self.passed + self.failed + self.skipped
        print()
        print("%d passed, %d failed, %d skipped (of %d)" %
              (self.passed, self.failed, self.skipped, total))
        return self.failed == 0


def make_restore_fn(snapshot):
    payload = {k: snapshot[k] for k in LIVE_FIELDS if k in snapshot}

    def restore(ctx):
        r = put_json(ctx, "/api/settings", payload)
        assert_status(r, 200)
        assert r.json().get("ok") is True

    return restore


PHASE_A = [
    ("page: GET / serves gzip html", test_index_page_gzip),
    ("state: GET /api/state works without auth", test_state_unauth),
    ("auth: every protected endpoint returns 401 without a session", test_unauthenticated_401),
    ("auth: wrong password is rejected", test_login_wrong_password),
    ("auth: a lower-case password still logs in", test_login_case_insensitive),
    ("auth: GET /login?t=bogus redirects to /?qr=expired, no cookie", test_qr_login_bogus_token),
    ("auth: a garbage session cookie is rejected", test_garbage_cookie_rejected),
]

PHASE_C = [
    ("settings: PUT with wrong Content-Type is rejected (415)", test_put_wrong_content_type),
    ("settings: GET /api/settings never leaks passwords", test_settings_no_password_leak),
    ("settings: PUT with an unknown key is rejected (400)", test_put_unknown_key),
    ("settings: PUT with 'name' is rejected (400) -- there is no device name",
     test_put_name_rejected),
    ("settings: PUT rejects dwell_s outside 0-15", test_put_dwell_bounds),
    ("settings: PUT rejects brightness outside 10-100", test_put_brightness_bounds),
    ("settings: an invalid PUT changes nothing (atomic)", test_put_atomicity),
    ("settings: a valid PUT round-trips live fields", test_put_live_roundtrip),
    ("coins: PUT with a malformed id is rejected", test_put_coins_bad_id),
    ("coins: PUT with 0 coins is rejected", test_put_coins_zero),
    ("coins: PUT with 16 coins is rejected", test_put_coins_too_many),
    ("coins: PUT with a duplicate coin is rejected", test_put_coins_duplicate),
    ("coins: PUT with an exchange-unknown coin is rejected", test_put_coins_unknown_exchange),
    ("coins: PUT adds a new valid coin, label upper-cased", test_put_coins_add_valid),
    ("status: /api/status has the expected shape", test_status_shape),
    ("scan: /api/scan returns a network list", test_scan),
    ("export/import: round trip changes nothing; Wi-Fi passwords included", test_export_import_roundtrip),
    ("settings: an oversize body is rejected (413)", test_oversize_body),
    ("login: an oversize body is rejected (413)", test_login_oversize_body),
    ("settings: a wide flat array under 4KB is rejected (400)", test_wide_flat_array_rejected),
    ("settings: limits and range_names reported; orientation_sensor is a bool",
     test_settings_limits_and_sensor),
    ("settings: JSON nested past the limit is rejected (400)", test_json_nesting_limit),
    ("wifi: invalid requests are refused before anything is saved",
     test_wifi_validation_rejects_before_saving),
    ("restart: refused without a JSON content type (415)", test_restart_needs_json),
    ("import: a network with no known password is skipped, with a warning",
     test_import_skips_unknown_network),
]


def main():
    ap = argparse.ArgumentParser(
        description="Integration test for the valumi web panel, over the LAN.")
    ap.add_argument("--host", required=True, help="device hostname or IP, e.g. valumi.local")
    ap.add_argument("--password", required=True, help="the panel password already set on the device")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--firmware", help="path to build/valumi.bin; enables the OTA test")
    ap.add_argument("--skip-lockout", action="store_true",
                    help="skip the ~30s lockout test")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help="default per-request timeout in seconds (default: %(default)s)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request/response")
    args = ap.parse_args()

    client = Client(args.host, args.port, timeout=args.timeout, verbose=args.verbose)
    ctx = Ctx(client, args)
    runner = Runner(verbose=args.verbose)

    print("valumi web panel test -- http://%s:%d" % (args.host, args.port))
    print()

    for name, fn in PHASE_A:
        runner.run(ctx, name, fn)

    runner.run(ctx, "auth: correct password logs in (Set-Cookie, HttpOnly, SameSite)",
              test_login_correct)

    if ctx.sid:
        # A ticker that has just booted is reachable before it is ready: the
        # clock takes a while to sync. The coin and status tests need it
        # ready, so wait for that first.
        deadline = time.time() + 120
        while time.time() < deadline:
            try:
                st = get(ctx, "/api/status", timeout=10).json()
                if st.get("clock_synced"):
                    break
            except Exception:
                pass
            time.sleep(3)
        else:
            print("WARNING: ticker not ready after 120 s -- coin tests may fail")

        try:
            ctx.snapshot = get(ctx, "/api/settings", timeout=10).json()
        except Exception as e:
            print("WARNING: could not snapshot settings for restore: %s" % e)
            ctx.snapshot = None

        try:
            for name, fn in PHASE_C:
                runner.run(ctx, name, fn)
        finally:
            if ctx.snapshot is not None:
                runner.run(ctx, "cleanup: restore live settings to their original values",
                          make_restore_fn(ctx.snapshot))
            else:
                print("WARNING: no snapshot was taken -- settings were NOT restored automatically")

        if args.skip_lockout:
            runner.skip("auth: lockout after 5 wrong passwords, clears after 30s",
                       "--skip-lockout")
        else:
            runner.run(ctx, "auth: lockout after 5 wrong passwords, clears after 30s",
                      test_lockout)

        if args.firmware:
            try:
                ctx.ota_before_partition = get(ctx, "/api/status", timeout=10).json()["system"]["partition"]
            except Exception as e:
                print("WARNING: could not read the current partition before OTA: %s" % e)
            runner.run(ctx, "ota: a non-firmware upload is rejected", test_ota_reject_garbage)
            runner.run(ctx, "ota: a real firmware upload is accepted and boots", test_ota_upgrade)
        else:
            runner.skip("ota: upload tests", "no --firmware given")

        runner.run(ctx, "auth: logout invalidates the session", test_logout)
    else:
        print()
        print("Login failed -- skipping every test that needs a session.")
        runner.skipped += len(PHASE_C) + 3

    ok = runner.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
