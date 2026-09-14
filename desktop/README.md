# LensLink Desktop

Use your iPhone as a **system-wide webcam and microphone** on Linux —
not just inside OBS. `lenslinkd` talks to the LensLink iOS app over the
same wire protocol as the OBS plugin, decodes the H.264/HEVC stream, and
publishes two virtual devices:

- **LensLink Virtual Camera** — a [v4l2loopback](https://github.com/umlaeute/v4l2loopback)
  device. Shows up in Chrome, Zoom, Teams, OBS, Cheese… anything that
  lists webcams. The capture node appears only while a stream is live
  (`exclusive_caps=1`).
- **LensLink Virtual Microphone** — a PipeWire source fed by the phone's
  mic (48 kHz stereo). Exists only while streaming; pick it in any
  input picker.

A tiny Qt tray item starts with your session and drives the daemon:
enable/disable, pick a phone (mDNS or USB), start/stop the camera on the
phone remotely, and a live status line
(`iPhone · 1920x1080 · 30 fps · 2.1 Mb/s · 43 ms`).

## Install (Arch)

```sh
# once, for the virtual camera module
sudo pacman -S v4l2loopback-dkms linux-headers
sudo modprobe v4l2loopback video_nr=9 card_label="LensLink Virtual Camera" exclusive_caps=1
```

Build and install the package:

```sh
cd desktop/packaging
makepkg -si
systemctl --user enable --now lenslinkd.service
```

The daemon now starts in the background at every login; the tray
autostarts with it (`/etc/xdg/autostart`).

## Usage

1. Open LensLink on the iPhone and leave it in the foreground (iOS only
   allows camera capture while the app is on screen — that's an iOS
   rule, not fixable here). For the microphone, turn **Send phone mic
   to OBS** on in the app's **Options** once — it persists.
2. The tray's **Enabled** toggle (on by default) dials the phone. The
   daemon auto-starts the camera via remote start, and the video and
   mic appear system-wide.
3. Pick a device in any app: camera = *LensLink Virtual Camera*,
   microphone = *LensLink Virtual Microphone*.

Hyprland/waybar users: make sure waybar's `tray/systray` module is
enabled to see the item.

## Configuration

`~/.config/lenslink/config.json` — written for you by the tray:

```json
{
  "enabled": true,
  "remote_start": true,
  "mic": true,
  "usb": false,
  "usb_udid": "",
  "host": "",
  "device": ""
}
```

- `host` — phone IP; empty = autodetect via mDNS each dial.
- `usb` — connect through usbmuxd instead of Wi-Fi (lower latency, no
  network, charges the phone).
- `device` — v4l2loopback path; empty = autodetect by card label.
- `remote_start` — start the phone's camera automatically when the app
  is reachable and idle. A manual stop always sticks until the phone
  becomes unreachable again.

## Scripting

The daemon answers one JSON request per line on
`$XDG_RUNTIME_DIR/lenslinkd.sock`:

```sh
printf '{"cmd":"status"}\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/lenslinkd.sock
# {"type":"status","enabled":true,"connected":true,"streaming":true,"name":"…","fps":30,...}
```

Commands: `status`, `enabled {on}`, `camera {on}`, `mic {on}`,
`use {usb,host,udid}`, `phones` (mDNS + USB scan), `quit`.

## Testing without a phone

`tools/fake-phone.py` emulates the iOS app (needs ffmpeg):

```sh
python3 tools/fake-phone.py --no-standby
```

## Architecture

```
iPhone app ──TCP (Wi-Fi / usbmuxd)──► lenslinkd
                                       ├─ session.c   dial loop, framing, timesync
                                       ├─ decode.c    libavcodec (software)
                                       ├─ vcam.c      v4l2loopback writer
                                       ├─ vmic.c      PipeWire Audio/Source
                                       └─ ipc.c       unix-socket JSON API
                                            ▲
                       lenslink-tray (Qt6) ─┘  1 Hz status, commands
```

`session.c` is distilled from the OBS plugin's `ios-camera-source.c`;
`usbmux.c`, `mdns.c` and `protocol.h` are shared verbatim (an
`obs-shim/` header stand-in compiles them outside OBS). One streaming
thread, no timers; audio meets PipeWire in a single-producer
single-consumer ring.

## Troubleshooting

- **No "LensLink Virtual Camera" device** — the module isn't loaded:
  `sudo modprobe v4l2loopback ...` (see Install), or point `device` at
  an existing loopback device.
- **Daemon runs but tray says "Daemon not running"** — the tray runs in
  your graphical session; `systemctl --user status lenslinkd` for the
  daemon's side.
- **Video works, mic missing** — turn **Send phone mic to OBS** on in
  the app's **Options** (persists across launches). The tray shows the
  mic's on/off state while connected. The daemon also understands a
  `mic` config key that asks the phone to stream its mic (a
  `send_mic` control command) — ready for a future app build; today's
  app ignores it.
- **Status stalls at "Looking for a phone…"** — open the iOS app
  foregrounded, and check *Local Network* permission for LensLink.
