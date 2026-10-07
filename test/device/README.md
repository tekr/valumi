# Web panel device test

`test_panel.py` drives a real, already-flashed ticker's web panel over the
LAN and checks its HTTP API. It talks stdlib-only Python (`http.client`,
`json`, `gzip`, `argparse`) -- no `requests`, nothing to `pip install`.

## Prerequisites

- A ticker on the network, in normal (online) mode, reachable at `--host`.
- Its panel password, for `--password`. The ticker generates it; hold the
  button for 5 seconds and the login screen shows it. Case does not matter.
- `.local` mDNS resolution works out of the box on macOS/Windows; on Linux
  you may need `avahi-daemon`, or just pass the device's IP address instead.

## Usage

```sh
python3 test_panel.py --host valumi.local --password <panel password>
python3 test_panel.py --host 192.168.1.42 --password K7RM3 -v
python3 test_panel.py --host valumi.local --password K7RM3 --skip-lockout
python3 test_panel.py --host valumi.local --password K7RM3 \
    --firmware ../../build/valumi.bin
```

Flags:

- `--host` (required) -- hostname or IP.
- `--password` (required) -- the panel password shown on the ticker.
- `--port` -- default 80.
- `--firmware PATH` -- if given, also uploads that `.bin` over `/api/ota`
  (a garbage-file rejection check, then the real upload, then waits for the
  device to reboot and checks it came back on the other OTA slot). Omit it
  and the OTA test is skipped -- there is no way to exercise OTA without
  actually reflashing the device.
- `--skip-lockout` -- skip the lockout test, which otherwise takes just
  over 30 seconds (5 wrong passwords, then a 31s wait for the lockout to
  clear).
- `--timeout SECONDS` -- default per-request timeout (12s). Several
  individual requests use longer timeouts of their own (coin/exchange
  lookups up to 35s, Wi-Fi scan up to 30s, OTA upload/reboot up to 90s+),
  since the device is small, single-core, and sometimes has to ask an
  exchange over the internet before it can answer.
- `-v` / `--verbose` -- log every request and response.

Exit status is non-zero if any test failed.

## What it does *not* touch

The test snapshots `GET /api/settings` at the start and restores the live
fields (`coins`, `dwell_s`, `orientation`, `brightness`, the transition
settings, `range`, `chart_style`, `night`, `tz_offset_min`) at the end, in a
`finally` block, so a run leaves the device the way it found it --
regardless of whether every test passed.

It never changes the Wi-Fi networks or restarts the device outside of the
optional OTA test:

- `POST /api/wifi` is only called with requests the ticker must refuse
  before saving anything (an empty list, a bad static address, an unknown
  key), and `POST /api/restart` only with a content type it rejects.
- The import test sends the ticker's own exported networks plus one it has
  no password for, which is skipped, so the list comes back unchanged.
- `POST /api/ota` is only exercised when you explicitly pass `--firmware`.

## Coverage

- Page: `/` serves gzip HTML.
- `/api/state` works without a session and reports `mode: "online"`,
  `hostname: "valumi"`, a non-empty `version`, and a boolean `logged_in`.
- Every protected endpoint (`/api/settings` GET/PUT, `/api/status`,
  `/api/scan`, `/api/export`, `/api/import`,
  `/api/wifi`, `/api/ota`, `/api/restart`) returns 401 without a session,
  before anything else about the request is checked.
- Login: wrong password -> 401; correct password -> 200 with a
  `Set-Cookie: sid=...; HttpOnly; SameSite=Strict`; the same password
  lower-cased still logs in (the panel password is compared
  case-insensitively); a garbage/foreign `sid` cookie is rejected;
  `GET /login?t=<bogus>` redirects to `/?qr=expired` with no cookie set.
- `PUT /api/settings` enforces `Content-Type: application/json` (415
  otherwise), rejects unknown keys (400, naming the key), rejects out-of-range `dwell_s`/
  `brightness`, and is atomic -- an invalid combined PUT changes nothing.
- A valid `PUT /api/settings` round-trips `dwell_s`, `brightness`, `range`,
  `chart_style`, `transition`, `transition_direction`, `orientation`,
  `night`, and `tz_offset_min`, with `restart_needed: false` and the
  resulting `settings` in the response body.
- Coins, through `PUT /api/settings`: a malformed id (400), adding a new
  valid coin (label upper-cased), an exchange-unknown coin
  (400, "does not list"), duplicates (400), 0 coins (400), 16 coins (400,
  one past the 15-coin limit).
- `/api/status` shape: exactly `wifi{mode,ssid,ip,rssi}`,
  `system{version,heap_min_free,partition}`, and `clock_synced`.
- `/api/scan` returns a network list (tolerates one 503 "radio busy" retry).
- `/api/export` carries each Wi-Fi password but not the panel password;
  importing it back is a
  no-op (`restart_needed: false`, settings unchanged, `settings` present in
  the response).
- `GET /api/settings` reports `limits` (`max_coins: 15`, `max_nets: 4`,
  `dwell_min: 0`, `dwell_max: 15`, plus the brightness/fade/transition/
  label/inst/ssid/wifi-password bounds) and
  `range_names: ["1D", "7D", "30D"]`; `orientation_sensor` is a boolean.
- An oversize body to `/api/settings` (>4096 bytes) is rejected with 413;
  an oversize login body (>256 bytes) is also rejected with 413; a wide
  flat JSON array (~1000 elements, well under 4096 bytes) to
  `/api/settings` is rejected with 400. Oversize refusals can close the
  connection before answering, which the test treats as an equally valid
  pass.
- JSON nested past 8 levels deep is rejected with 400.
- `POST /api/wifi` only accepts `{"networks": [...]}` -- an empty list, an
  invalid static IP, and any unknown top-level key are all rejected before
  anything is saved.
- Lockout: 5 wrong passwords lock out further attempts (429, including a
  *correct* password); the lock clears after 30s.
- `POST /api/restart` requires a JSON content type (415 otherwise).
- Importing a network with no known password is skipped with a warning,
  leaving the device's own networks untouched.
- `POST /api/logout` invalidates the session; the same cookie then gets 401.
- Optional, only with `--firmware`: a non-firmware upload is rejected
  (400) without disturbing the device; a real upload is accepted, the
  device reboots, and it comes back on the other OTA partition.

## Notes from reading the server source

A couple of things worth knowing about while working on this API:

- The login rate limiter (`auth_core.c`) is a single global counter, not
  per-client. Any wrong password from *any* source counts towards the
  lockout, and the lockout blocks *everyone*, including someone typing the
  right password. That's an accepted tradeoff for a single-owner device,
  but it does mean the lockout test in this file affects the whole device
  for the ~30s it takes to clear.
- Refusals sent before the request body is read (413, 415, and 401 for a
  body over 4 KB) close the connection rather than reading the body
  (`web_panel.c`'s `refuse()`), so the client may see a reset/closed
  connection instead of a clean HTTP response. The test's `refused()` helper
  treats either as a pass. A 401 for a smaller body is sent after reading it,
  so the browser always gets it.
