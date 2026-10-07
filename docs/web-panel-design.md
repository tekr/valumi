# valumi: web control panel

Status: shipped, release 1.3. Targets both 1.47" C6 boards (4 MB plain,
8 MB touch) from one binary.

## Goals

- Set up a brand-new ticker without a USB cable or an existing network.
- Change every user-facing setting from a browser, live, without a reboot
  (Wi-Fi networks are the only exception).
- Keep Wi-Fi passwords out of the repo: `app_config.h` holds only factory
  defaults.
- Update firmware from the browser.

## Modes

| Mode | Entered when | Panel password |
|---|---|---|
| **Setup** (device is its own hotspot) | no saved networks (first boot, after factory reset), or no saved network answered for `APP_SETUP_FALLBACK_S` (120 s) | not needed -- the on-screen hotspot passphrase is the gate |
| **Normal** (client of a saved network) | any saved network connects | required |

Setup mode keeps retrying the saved networks in the background, so a ticker
that fell back because the router was down returns to Normal mode without
help. While a phone is connected to the hotspot it only looks, every 60 s,
with a scan that keeps the phone connected, and joins only when a saved
network is actually there: joining moves the radio to that network's
channel, and the hotspot with it, which drops the phone.

Setup mode stays reachable even when entered as a fallback from Normal mode
(owner decision): anyone who has seen the hotspot passphrase on the screen
can use it to change anything, Wi-Fi and firmware included, during an
outage. The passphrase is the gate either way.

**Scan** works in Setup mode, phone connected or not: the radio runs AP+STA
and returns to the hotspot's channel between the channels it visits, so the
phone stays associated.

### Setup mode

- SSID `VALUMI-XXXX` (fixed prefix plus the last 4 hex digits of the MAC, so
  two tickers side by side differ), WPA2 with a passphrase that is
  regenerated every time setup mode starts -- one seen on screen is no use
  later.
- Screen shows a Wi-Fi QR code (`WIFI:T:WPA;S:..;P:..;;`) carrying the
  passphrase, so a phone camera joins without anything being typed; the
  SSID and passphrase are also printed beside it for laptops.
- Captive portal: a DNS server answers every query with the hotspot's own
  address, and `GET /capport` serves a small RFC 8908 document (also
  advertised via DHCP option 114) so modern phones open the panel by
  themselves without a user tapping a notification.
- The hotspot's address is `4.3.2.1`, not a private address. Android decides
  "no internet" without even trying when its connectivity-check hostname
  resolves to a private address (confirmed on a Galaxy Z Fold 7: it looked
  the name up and never made the request); a public-looking address gets
  the redirect and the sign-in prompt. The hotspot reaches nothing beyond
  the ticker, so borrowing the address harms no one (WLED does the same,
  for the same reason).
- Setup mode needs no login of its own: the hotspot passphrase is the only
  gate, in both the first-boot and fallback cases.

### Address / login screen ("CONNECTED" / "LOG IN")

Shown for `APP_ADDRESS_SCREEN_S` (5 s) on the first connection after boot
and on leaving setup mode: IP address, `http://valumi.local`, the panel
password, and a one-time login QR code.

The QR code encodes `http://<ip>/login?t=<token>`, so a phone camera opens
the panel already logged in -- seeing the screen means holding the device,
which is the same proof the hotspot passphrase relies on.

- Token: 128 random bits, generated fresh each time the screen is shown,
  kept in RAM only.
- Single use, valid for the time the screen is up plus 15 s, whichever
  comes first.
- The link never reaches the page's history: `/login` swaps it for the
  session cookie and redirects to `/`.
- **Show it again:** hold the button 5-10 s and release. A login screen
  shows for `APP_LOGIN_SCREEN_S` (20 s) with a fresh token and the manual
  details (URL, `valumi.local`, IP).
- The panel password remains the way in for anyone not holding the device.

### Button

One button (`button_seq.c`), timed by how long it is held:

| Held | Action | Feedback while held |
|---|---|---|
| < 1 s | brightness step, on release (the night level, at night) | none |
| reaches 1 s | cycle chart range, fired while still held | from 2 s: "keep holding for login details", counting down to 5 |
| released between 5 and 10 s | login screen for 20 s (QR + manual details) | at 5 s: "release now for login details / keep holding for factory reset", counting to 10 |
| reaches 10 s | factory reset fires immediately, without waiting for release | "Resetting..." |

In setup mode the hint only offers factory reset (there is no login to show).

Factory reset erases the whole NVS partition (`nvs_flash_erase()`, not just
a namespace) and reboots into setup mode. Nothing is tied to power-on,
because BOOT is GPIO9, a strapping pin: held at power-on it enters download
mode and the app never runs.

Brightness and chart range are the everyday actions, so they keep their
instant feel: any hold long enough to reach the login screen or a reset has
also changed the chart range once, at 1 s. Accepted; the range is not
restored afterwards.

## Settings

Stored in NVS as JSON (`settings_store.c`; see Compatibility). Defaults come
from `app_config.h`. **Live** = applied without a reboot.

| Setting | Default | Live | Notes |
|---|---|---|---|
| Wi-Fi networks (priority list, max 4) | none | no -- reboot | add / remove / reorder; **Scan** lists nearby SSIDs |
| per network: DHCP or static IP, mask, gateway, DNS | DHCP | no | static is per network: a home-LAN address is wrong on the hotspot |
| Coins (ordered, max 15) | OKB, BTC, ETH, SOL | yes | each new one checked against OKX; label defaults to the base currency |
| Page dwell (counted once the coin has slid in) | 8 s | yes | 0-15 s; 0 scrolls continuously |
| Orientation: Fixed checkbox | unchecked with an accelerometer, checked and greyed out without one | yes | keyed on `board_orientation_available()`, not on the board variant |
| Orientation when fixed | USB on right | yes | "USB on right" / "USB on left" |
| Brightness | `APP_BRIGHTNESS_DEFAULT` (30) | yes | the button still changes it; button changes are saved too |
| Night dim: on/off, start, end, level | off, 22:00-07:00 | yes | uses the existing SNTP clock plus the timezone below |
| Timezone | UTC+8 | yes | stored as minutes (UTC+8 = 480); no automatic daylight saving |
| Default chart range | 7D | yes | the button still cycles it |
| Chart style | closes | yes | closes (one point per candle) / highs & lows (each candle's extremes -- low then high for a candle that closed up, high then low otherwise -- keeping only swings over 4% of the chart's height, drawn as a monotone cubic that never overshoots a point) / close + range (the close line over a band from each low to its high) |
| Transition | slide | yes | slide (both pages move together) / cover (the new page moves in over the old one, which drifts and dims) / cascade (a slide, the title, the change figures and the chart leaving in turn) / wipe (neither page moves; an edge sweeps across) |
| Transition direction | horizontal | yes | horizontal / vertical; applies to every transition |
| Transition time | 680 ms | yes | 200-3000 ms |
| Fade style | crossfade | yes | crossfade / dip; slide and cascade only |
| Fade depth | 100% | yes | 0-100%, how far a page dims while changing; for cover, how far the covered page dims; unused by wipe |
| Panel password | generated | n/a | see below -- never set through this API |

Stored Wi-Fi passwords are write-only: the API never returns them; the page
shows "saved" and offers replace only.

There is no device name or hostname setting. The hostname and mDNS name are
fixed (`APP_HOSTNAME "valumi"`, panel at `http://valumi.local`), and the
setup hotspot's SSID prefix is fixed too.

### Panel password

Never chosen by anyone, and there is no change UI or API. Generated (5
characters, A-Z and 2-9, no look-alikes) on first boot and after a factory
reset, and always shown on the login screen and the "CONNECTED" screen --
because someone holding the ticker without a camera has no other way in.
Compared case-insensitively against what is typed (the screen shows upper
case; phone keyboards start lower case).

Stored and compared in the clear (`auth_password_ok()`), not hashed: it is
shown on the screen anyway, so hashing it in storage would protect nothing
that matters here.

A gitignored `main/wifi_secrets.h` can seed a panel password of the same
form, with networks, whenever a development device has no usable settings
(blank, or unreadable); the first boot after a factory reset
does not apply it.

## Web panel

- `esp_http_server`, HTTP only. A TLS server would need a second ~40 KB TLS
  context next to the OKX session, and the heap does not have room for
  both. Accepted risk: the panel password crosses the local network in the
  clear, for a desk ticker on a home network or the owner's own hotspot.
- One self-contained page (HTML/CSS/JS inline, no CDN, since setup mode has
  no internet), gzipped and embedded in the app image.
- Routes: `GET /`, `GET /logo.png`, `GET /login?t=`, `GET /capport`, `GET /api/state`,
  `POST /api/login`, `POST /api/logout`, `GET/PUT /api/settings`,
  `GET /api/status`, `GET /api/scan`,
  `POST /api/wifi`, `GET /api/export`, `POST /api/import`, `POST /api/ota`,
  `POST /api/restart`.
- Auth: login (password, or the one-time QR token) sets a random session
  token cookie (`SameSite=Strict`, kept in RAM, lost on reboot, up to 4
  sessions, idle ones dropped after 30 days). Login is rate-limited after 5
  consecutive failures (globally, 30 s lockout).
- Request limits: bodies up to 4 KB (256 B for `/api/login`), JSON nested
  no deeper than 8 levels with at most 256 elements, a whole-body deadline
  of 10 s (300 s for `/api/ota`) -- the server is one task with a small
  stack on a small heap, so a slow or oversized request is refused and the
  connection closed rather than held open.
- `GET/PUT /api/settings`: the GET response is the settings plus what the
  page needs to render and validate them -- `orientation_sensor`,
  `range_names`, and a `limits` object (`max_coins`, `max_nets`,
  `dwell_min`/`max`, `bright_min`/`max`, `fade_max`, `transition_min_ms`/
  `max_ms`, `label_max`, `inst_max`, `ssid_max`, `wifi_pw_min`/`max`), so
  the page validates by the same rules as the firmware. PUT never touches
  Wi-Fi networks, so it never needs a restart; it responds
  `{"ok","restart_needed":false,"warnings","settings"}`. New coins are
  checked against OKX before being accepted; one OKX does not list is
  refused, one that cannot be reached in time is accepted with a warning.
- `POST /api/wifi`: saves the network list and restarts.
- `GET /api/status` (authenticated, polled): `wifi{mode,ssid,ip,rssi}`,
  `system{version,heap_min_free,partition}`, `clock_synced`. The page shows
  a memory figure only when `heap_min_free` has dropped below 10 KB since
  boot; nothing else here (no uptime, hotspot client count, coin prices, or
  board name -- the owner decluttered the panel of status nobody needed day
  to day).
- `GET /api/export` / `POST /api/import`: settings as JSON, with each Wi-Fi
  password but not the panel password (each ticker keeps its own). A network
  listed without a password takes the ticker's own for that SSID; one it has
  none for is skipped (with a warning in the response), and the ticker keeps
  its own networks if none can be imported. It
  shares its response shape with PUT, except `restart_needed` is true when
  the imported networks differ from the current ones.
- `POST /api/ota`: streamed straight to the inactive slot, never buffered
  in RAM; checked for the ESP image magic, the chip, and the project name
  before anything is written. A full flash image (the release file, flashed
  at 0 over USB) is accepted too: the bytes before its app slot are skipped,
  so one download serves both a first install and updates.

## OTA

Partition table (`partitions.csv`), fixed for both boards and sized for the
smaller (4 MB) one -- the 8 MB board's upper half goes unused:

```
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x1E0000
ota_1,    app,  ota_1,   0x200000, 0x1E0000
```

Two 1.875 MB slots. The new image boots on probation
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) and confirms itself
(`firmware_confirm_when_settled()`) once the panel has been reachable --
joined a network, or brought up the setup hotspot -- for 60 s, so an image
that crashes on its first requests is still rolled back. It also confirms
immediately before any panel-initiated restart or update
(`firmware_confirm()`, called from the OTA upload handler and from the
restart path), since serving that request is proof enough that the image
works, and before a factory reset.

## Memory

The binding constraint is RAM, not flash. The OKX client allocates its
buffers per response, sized to it, rather than holding them for the life of
the connection; mbedTLS uses dynamic record buffers for the same reason.
Measured at 15 coins: lowest free heap since boot (including the TLS
handshake) is 48.5 KB quiet, and 24 KB with three phones loading the panel
at once; an overnight run at that load was stable. No page is ever built in
RAM -- it is served straight from flash.

## Code structure

- `main.c` -- the render loop, screen state machine, button handling, and
  wiring the other modules together.
- `page_transition.c` -- the frames of a page change: slide, cover, cascade
  and wipe, sideways or up and down, drawn from `ui.c`'s rows.
- `market.c` -- the net task: holds the exchange connection, polls prices
  and candles for the on-screen and next coin, and answers coin-list and
  coin-check requests from the panel on that same task.
- `market_core.c` -- the pure parts of market data handling (coin-list
  remapping, the stale-price threshold, candle-series merging), with no
  FreeRTOS or network dependency, so it is host-tested.
- `firmware.c` -- OTA probation and confirmation.
- `web_panel.c` -- the HTTP server and JSON API. It calls
  `market_check_coin()` and `market_clock_synced()` directly; there is no
  callback/hook layer between the panel and the market task.
- `settings.c` / `settings_json.c` / `settings_store.c` -- the settings
  struct with its validation (pure, host-tested), its JSON encoding (pure,
  host-tested), and its NVS storage.

## Compatibility

Settings are saved in NVS as JSON: the export format plus the Wi-Fi and
panel passwords. On boot the firmware reads it field by field. A field it
does not know is ignored, and a missing or invalid one keeps its default.
So an update that adds or drops a setting keeps every other setting, and a
new setting starts at its default. Fields a newer firmware saved inside a
network, coin or the night object are skipped the same way, so a rollback
keeps them too. Saved settings that cannot be read at all leave the ticker
on defaults for that boot, in setup mode, without being overwritten.

## Out of scope

HTTPS on the device; more than 4 networks; accounts or multiple users;
remote access from outside the LAN; changing the panel password.
