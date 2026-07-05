# kindle-koreader-airplay

A [KOReader](https://github.com/koreader/koreader) plugin that turns your Kindle into an AirPlay screen mirror receiver. Your Mac thinks the Kindle is a display. The Kindle renders the stream on its e-ink screen at ~0.5–2 FPS in grayscale.

## Why?

I DON'T KNOW. I just wanted to see if it would be possible. Turns out it is. Whether it's *useful* is a separate question.

## What it does

```
Mac (AirPlay sender)
  │  mDNS discovery (_airplay._tcp)
  │  HTTP/AirPlay control  :7000
  │  H.264 stream          :7100
  ▼
Kindle (KOReader plugin)
  │  libairplay_mirror.so  — C library: AirPlay protocol + H.264 decode
  │  airplay_ffi.lua       — LuaJIT FFI bindings
  │  main.lua              — KOReader plugin + e-ink renderer
  ▼
E-ink display (~0.5–1 FPS, grayscale)
```

The C library wraps [UxPlay](https://github.com/FDH2/UxPlay)'s `raop` implementation for the AirPlay/RTSP/FairPlay side, adds ffmpeg H.264 decode → gray8, and writes frames directly to `/dev/fb0` via Kindle's `mxcfb` EPDC ioctls (no KOReader blitbuffer involved). mDNS is a hand-rolled UDP multicast implementation — Kindle has no Bonjour.

## Requirements
- Jailbroken Kindle with KOReader installed
- Wi-Fi, on the same network as your Mac

## Install

Grab the zip matching your Kindle's firmware from [Releases](../../releases):

- `airplay.koplugin-sf-*.zip` — firmware ≤5.16.2.1.1 (Paperwhite 1-5, Voyage, Oasis 1-3)
- `airplay.koplugin-hf-*.zip` — firmware ≥5.16.3 (Paperwhite 6/12, current Scribe, or any older device updated past 5.16.2.1.1)

Not sure which one? Check Settings → Device Info on the Kindle, or `cat /etc/prettyversion.txt` over SSH.

- Unzip, then copy the plugin directory to your Kindle over USB/SSH:
  - `airplay.koplugin/` → `/Volumes/Kindle/extensions/koreader/plugins/`
- Restart KOReader.

## Building

```
make docker-kindle TARGET=sf   # firmware <=5.16.2.1.1
make docker-kindle TARGET=hf   # firmware >=5.16.3
```

Both use Docker so no local toolchain install is needed (Dockerfile.sf / Dockerfile.hf). See the top of the [Makefile](Makefile) for native cross-compiler setup on Mac. CI builds and releases both variants automatically on every `v*` tag push.

## Usage

1. Connect Kindle and Mac to the same Wi-Fi network
2. In KOReader: **Menu → Tools → AirPlay Mirror → Start AirPlay receiver**
3. On Mac: **System Settings → Displays → Add Display → AirPlay Display**
4. Select **"Kindle AirPlay"** from the list
5. That's it

### Refresh rate options

**Menu → Tools → AirPlay Mirror → Refresh rate**
- **0.5 FPS (2000 ms)** — least ghosting, default
- **1 FPS (1000 ms)** — faster, more ghosting

E-ink is e-ink. Don't expect much.

## Known limitations

- **Grayscale only** — e-ink is grayscale; color content is dithered
- **~0.5–1 FPS** — e-ink physics, not a software problem
- **No audio** — Kindle has no speakers; audio is intentionally not implemented
- **macOS 12–14 tested** — AirPlay protocol could change in future macOS
- **No AirPlay 2 multiroom** — screen mirroring only
- **Kindle mxcfb EPDC v2** — tested on an OLD Paperwhite 5th Gen, I have no idea how well would it work for any other device!!
- **Two ABI builds required** — Amazon switched their toolchain from soft- to hard-float at firmware 5.16.3, so sf and hf builds are not interchangeable; grab the right one (see Install above).

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) — AirPlay receiver library (git submodule)
- [RPiPlay](https://github.com/FD-/RPiPlay) — original open-source AirPlay receiver
- [Unofficial AirPlay spec](https://nto.github.io/AirPlay.html)

## License

This project wraps UxPlay which is GPLv3. This project is therefore also GPLv3.
