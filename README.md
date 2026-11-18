# Pringle Desk Panel

A touch-screen status panel for a homelab, built on the ESP32-2432S028R ("Cheap Yellow Display", 2.8" 320×240). It shows your servers, internet, ad blocker and power use at a glance, with Pringle the pixel alpaca as the mascot.

Pringle reflects the state of the homelab:

- **Running** when everything is fine. He runs faster when the servers are busy.
- **Worried**, under storm clouds and rain, when something is wrong.
- **Asleep**, under a night sky, when the homelab can't be reached.

The on-board RGB LED agrees with him: green when all is fine, red on a problem, amber when ad blocking is off, and blue when there's no data or the data is out of date.

## Screens

| Tab | What it shows |
|---|---|
| **Home** | Pringle in a little scene that follows the time of day (sunrise, day, sunset, night), a status pill, and cards for CPU and temperature (with 16-minute sparklines) and ads blocked today. |
| **Server** | Every server as a tile in a 2×2 grid, 4 per page, up to 12 servers. Offline servers come first, then servers with problems. Tap `<` / `>` to page. Tap a tile to see big CPU / RAM / temperature bars, the problem text and 16-minute graphs. |
| **Ads** | Share of lookups blocked today, as a ring, plus the blocked and total counts. |
| **Alerts** | The list of current problems, or "Nothing wrong" with internet latency, device count and power use. |

The top bar shows the time, internet latency, ad blocking state and WiFi signal. If a request fails, the last data stays on screen and the top bar says how long ago it was updated.

## Hardware

- ESP32-2432S028R (ESP32-WROOM, ILI9341 display, XPT2046 resistive touch)
- A USB cable. That's all.

| Part | Pins |
|---|---|
| Display (HSPI) | MOSI 13, MISO 12, SCK 14, CS 15, DC 2, backlight 21 |
| Touch (separate SPI) | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| RGB LED (active low) | R 4, G 16, B 17 |

## Setup

1. Install [PlatformIO](https://platformio.org/) (the VS Code extension or the `pio` command line).
2. Clone this repository.
3. Create your secrets file:
   ```sh
   cp include/secrets.example.h include/secrets.h
   ```
   Fill in your WiFi name and password, the URL of your dashboard's status JSON, and the dashboard's viewer user and password. `include/secrets.h` is git-ignored, so it stays on your computer.
4. Plug in the board and flash it:
   ```sh
   pio run -t upload
   pio device monitor
   ```

The ESP32 can't join Tailscale, so `API_URL` must be the server's address on your home network, and the server must accept connections from the panel on that port.

## The data it reads

The panel polls `API_URL` every 30 seconds with HTTP basic auth. The dashboard uses a self-signed certificate, so the panel connects with `WiFiClientSecure::setInsecure()`. The JSON looks like this, and any number may be `null`:

```json
{
  "ok": true,
  "problems": [],
  "devices": [
    { "name": "Lenovo", "online": true, "cpu": 2.9, "mem": 15.9, "temp": 36.0, "problems": [] }
  ],
  "internet": { "online": true, "ms": 4 },
  "ads": { "on": true, "blocked": 7605, "queries": 50108 },
  "power": { "watts": 40 }
}
```

Other fields are ignored. When the server answers `401` the panel waits 10 minutes before trying again, and after `429` it waits 5 minutes, so it never triggers the dashboard's lockout.

## Project layout

| Path | What it is |
|---|---|
| `src/main.cpp` | The firmware: network task, screens, touch and animation |
| `platformio.ini` | Board, libraries and display pin configuration |
| `include/secrets.example.h` | Template for your WiFi and dashboard settings |
| `include/sprites.h` | Pringle's sprites as RGB565 arrays (generated) |
| `tools/sprites.py` | Regenerates `sprites.h` from the Pringle Mac app's install script |
| `design/pringle-panel.html` | The first clickable mockup of the screen |

To regenerate the sprites (needs Pillow):

```sh
python3 tools/sprites.py path/to/pringle-mac.sh
```

## Previewing many servers

Set `DEMO_SERVERS` near the top of `src/main.cpp` to a number above 0 to add fake servers (`demo-1`, `demo-2`, …). One is offline and one has a problem, so you can see every tile style. Set it back to `0` for normal use.

## Versions

| Tag | Design |
|---|---|
| `v1-basic` | First working version: plain layout with live data |
| `v2-design-saved` | Same layout with Pringle's sky scene, pixel icons, rounded cards and an icon tab bar |
| `main` | Server grid with paging and a detail view, times of day, birds, flowers, fireflies and sparklines |

## Troubleshooting

- **Colors look inverted or wrong.** Some boards (often the version with two USB ports) use an ST7789 display. In `platformio.ini`, replace `-DILI9341_2_DRIVER=1` with `-DST7789_DRIVER=1`.
- **Upload fails with "No more data to read from the serial port".** Lower `upload_speed` in `platformio.ini`. 460800 works on most boards.
- **"Can't reach the homelab".** WiFi works but the server didn't answer. Check `API_URL`, and check that the server accepts connections from the panel's IP on that port.
- **"Wrong viewer password".** Fix `API_PASS` in `include/secrets.h` and flash again.
- **Taps land in the wrong place.** Adjust `TX_MIN`, `TX_MAX`, `TY_MIN` and `TY_MAX` in `src/main.cpp`.
