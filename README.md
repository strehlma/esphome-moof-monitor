# ESPHome Moof Monitor

**An ESPHome monitor for VanMoof S3 / X3 e-bikes.**

Read battery level, odometer, lock state and more from **VanMoof S3 / X3** e-bikes
over Bluetooth Low Energy with an ESP32, and bring the values into
[Home Assistant](https://www.home-assistant.io/) through [ESPHome](https://esphome.io/).
No VanMoof cloud and no phone are needed once the bike's encryption key is known.

> Unofficial project, not affiliated with VanMoof. See [Disclaimer](#disclaimer).

The project contains:

| Path | What it is |
|---|---|
| `components/vanmoof_ble/` | ESPHome external component that connects, logs in and reads the bike |
| `vanmoof/bike.yaml` | Package included once per bike |
| `vanmoof-monitor.yaml` | Example device configuration |
| `secrets.example.yaml` | Template for your `secrets.yaml` |
| `blueprints/automation/vanmoof_monitor/vanmoof_charge.yaml` | Home Assistant blueprint: charge the bike with a smart plug |
| `dashboard/vanmoof-bikes-card.yaml` | Dashboard card (button-card) that finds the bikes on its own |
| `docs/BLE_PROTOCOL.md` | Notes on the VanMoof BLE protocol |

## What you get in Home Assistant

Each bike becomes its own device (a sub-device of the monitor) with these entities:

| Entity | Type | Notes |
|---|---|---|
| Battery | sensor, % | main battery; reported on every reading |
| Distance | sensor, km | odometer |
| Lock State | binary sensor | on = locked |
| Charging | binary sensor | |
| In Range | binary sensor | the monitor receives the bike, or is connected to it |
| Alarm | text | Disarmed, Armed, Alarm 1-3 |
| Alarm Setting | text, diagnostic | Off, Manual, Automatic |
| Module Battery | sensor, diagnostic | battery of the bike's electronics module |
| Module State | text, diagnostic | On, Off, Standby, Sleeping, Tracking, ... |
| Error Code | sensor, diagnostic | 0 = no error |
| Firmware | text, diagnostic | bike firmware version |
| Frame Number | text, diagnostic | the bike's frame (serial) number |
| Signal Strength | sensor, diagnostic | RSSI from advertisements |

A bike is read about every 5 minutes while it is in range. Values keep their last
state while the bike is away.

## Hardware

- Any ESP32 with Bluetooth (tested with a classic ESP32 dev board, `esp32dev`).
- Place it within Bluetooth range of where the bikes are parked.
- One ESP32 handles several bikes; it reads them one after another.

Tested with two VanMoof S3 bikes on firmware 1.09.03. The X3 uses the same
protocol and should work, but has not been tested. Older models (S2/X2 and earlier)
and the S5/A5 use a different protocol and are **not** supported.

## Setup

### 1. Get the bike's MAC address and encryption key

The bike only gives out its data after a login with its own 128-bit key. Each
key has a key ID (usually `1` for the owner's key). See
[Getting the bike's encryption key](#getting-the-bikes-encryption-key) below.

### 2. Add the component to ESPHome

There are two ways. Option A needs no files copied and is the easiest to update.

**Option A: load it from GitHub (recommended).** Copy `vanmoof-monitor.yaml` into
your ESPHome configuration folder (in Home Assistant usually `/config/esphome/`)
and replace its `packages:` and `external_components:` blocks with:

```yaml
packages:
  vanmoof:
    url: https://github.com/strehlma/esphome-moof-monitor
    ref: v1.11.0          # pin a release; change it to update
    files:
      - path: vanmoof/bike.yaml
        vars:
          bike_id: bike1
          bike_name: "Bike 1"
          mac_address: !secret bike1_mac
          encryption_key: !secret bike1_encryption_key
          key_id: !secret bike1_key_id
      # one more entry like the above per additional bike

external_components:
  - source: github://strehlma/esphome-moof-monitor@v1.11.0
    components: [vanmoof_ble]
```

Keep both `ref` values on the same release: the package checks at build time that
its version matches the component.

**Option B: copy the files.** Copy these into your ESPHome configuration folder:

```
components/vanmoof_ble/
vanmoof/bike.yaml
vanmoof-monitor.yaml
```

### 3. Create `secrets.yaml`

Copy `secrets.example.yaml` to `secrets.yaml` (or add the keys to your existing
one) and fill in WiFi, API key, OTA password and, per bike, MAC address,
encryption key and key ID. **Never publish `secrets.yaml`.**

### 4. Adjust the bikes

In `vanmoof-monitor.yaml`, keep one bike entry per bike and give each a
`bike_name` (shown in Home Assistant). `bike_id` must be unique and may only
contain letters, digits and underscores. Remove the second block if you have
only one bike, or copy it for a third.

### 5. Flash

Install the firmware from the ESPHome dashboard (first time via USB), then add
the device in Home Assistant when it is discovered.

## Getting the bike's encryption key

The key was issued by VanMoof and stored in your VanMoof account. Export it
while the account is still reachable, and keep a copy somewhere safe. Options:

- **[VanMoof Encryption Key Exporter](https://github.com/grossartig/vanmoof-encryption-key-exporter)**:
  log in with your VanMoof account and it shows the bikes with MAC address,
  encryption key and key ID.
- **[pymoof](https://github.com/quantsini/pymoof)**: a Python library for VanMoof
  bikes that can also fetch the key from the account; see its README.
- **[Bikey](https://bikey-app.cowboy.com/)**: an app for VanMoof riders that can
  back up the bike's key.

Notes:

- The encryption key is 32 hexadecimal characters (16 bytes).
- Use the MAC address in the form `AA:BB:CC:DD:EE:FF`. A BLE scanner app (e.g.
  nRF Connect) shows it as well.
- Treat the key like a house key: anyone who has it can connect to the bike.

## Dashboard card

`dashboard/vanmoof-bikes-card.yaml` contains two
[button-card](https://github.com/custom-cards/button-card) templates. The card
lists every bike of the device "VanMoof Monitor" with battery bar, range status,
lock, alarm, module battery, error code, distance and firmware, plus an optional
7-day battery graph (needs [mini-graph-card](https://github.com/kalkih/mini-graph-card)).
No entity IDs have to be entered. Setup instructions are at the top of the file.

## Blueprint: charge with a smart plug

`vanmoof_charge.yaml` switches a smart plug for the charger on when the battery
drops below a lower limit and off again above an upper limit (default 30 % / 80 %),
when charging has ended or when the bike has left. An optional `input_boolean`
requests one full charge to 100 %. Notifications for low battery, full charge and
"plug on but the bike does not charge" can be added as actions.

Import it in Home Assistant under *Settings → Automations & scenes → Blueprints →
Import blueprint* with this URL:

```
https://github.com/strehlma/esphome-moof-monitor/blob/main/blueprints/automation/vanmoof_monitor/vanmoof_charge.yaml
```

or copy the file to `/config/blueprints/automation/vanmoof_monitor/`.

## Troubleshooting

- **In Range stays off:** the monitor does not receive the bike. A phone with the
  VanMoof app (or another app) connected to the bike stops it from advertising;
  close the app. A bike in deep sleep wakes up when it is moved.
- **Values do not update although the bike is in range:** after about a day
  without use the bike goes into a Bluetooth standby in which the login has no
  effect until it is moved. The component detects this and waits longer between
  attempts.
- **More detail in the log:** set `logger: level: DEBUG` and flash again.
- **Sharing logs:** the log shows the frame number on INFO level. Remove it before
  posting a log publicly.
- **Bike plays a sound when the monitor connects:** a new connection can wake the
  bike. `keep_connected: true` (set in `vanmoof/bike.yaml`) keeps the link open
  between readings, so this happens only rarely.

## Credits

The protocol knowledge (UUIDs, login sequence, data formats) comes from these
projects. This project contains no code copied from them; the component is an
independent C++ implementation for ESPHome.

- [pymoof](https://github.com/quantsini/pymoof) (MIT)
- [VanMoofKit](https://github.com/SvenTiigi/VanMoofKit) (MIT)
- [ha-vanmoof](https://github.com/BobMcGlobus/ha-vanmoof) (MIT)
- [vanmoof-tools](https://github.com/chwdt/vanmoof-tools) (reverse-engineering notes)

## Disclaimer

This is an unofficial community project. It is not affiliated with, endorsed by or
sponsored by VanMoof. "VanMoof" is a trademark of its respective owner and is used
here only to describe which bikes this project works with. The protocol was
reverse-engineered by the community and may change with a firmware update. Use at
your own risk.

## License

MIT, see [LICENSE](LICENSE).
