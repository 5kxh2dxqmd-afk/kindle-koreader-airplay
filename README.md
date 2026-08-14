# kindle-koreader-airplay

A [KOReader](https://github.com/koreader/koreader) plugin that turns your Kindle into an AirPlay screen mirror receiver. Your Mac thinks the Kindle is a display. The Kindle renders the stream on its e-ink screen at ~0.5–2 FPS in grayscale.

> **Why?** I don't know. I just wanted to see if it would be possible. Turns out it is. Whether it's *useful* is a separate question.

---

## How it works

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

---

## Requirements

- Jailbroken Kindle with KOReader installed
- Wi-Fi, on the same network as your Mac

---

## Install

Grab the zip matching your Kindle's firmware from [Releases](../../releases):

| Zip | Firmware | Devices |
|-----|----------|---------|
| `airplay.koplugin-sf-*.zip` | ≤ 5.16.2.1.1 | Paperwhite 1–5, Voyage, Oasis 1–3 |
| `airplay.koplugin-hf-*.zip` | ≥ 5.16.3 | Paperwhite 6/12, current Scribe, or any older device updated past 5.16.2.1.1 |

Not sure which one? Check **Settings → Device Info** on the Kindle, or `cat /etc/prettyversion.txt` over SSH.

Unzip, then copy the plugin directory to your Kindle over USB or SSH:

```
airplay.koplugin/  →  /Volumes/Kindle/extensions/koreader/plugins/
```

Restart KOReader.

---

## Usage

1. Connect Kindle and Mac to the same Wi-Fi network
2. In KOReader: **Menu → Tools → AirPlay Mirror → Start AirPlay receiver**
3. On Mac: **System Settings → Displays → Add Display → AirPlay Display**
4. Select **"Kindle AirPlay"** from the list

> [!NOTE]
> Your Kindle may default to a small resolution when connecting from a Mac, making it difficult to fit things on screen. To fix: **Settings → Display → Kindle AirPlay → Optimize for**, then select your Mac, not the Kindle

### Refresh rate

**Menu → Tools → AirPlay Mirror → Refresh rate**

| Mode | Interval | Notes |
|------|----------|-------|
| 0.5 FPS | 2000 ms | Least ghosting — default |
| 1 FPS | 1000 ms | Faster, more ghosting |

E-ink is e-ink. Don't expect much.

---

## Building

```sh
make docker-kindle TARGET=sf   # firmware ≤5.16.2.1.1
make docker-kindle TARGET=hf   # firmware ≥5.16.3
```

Both use Docker — no local toolchain needed (`Dockerfile.sf` / `Dockerfile.hf`). See the top of the [Makefile](Makefile) for native cross-compiler setup on Mac. CI builds and releases both variants automatically on every `v*` tag push.

---

## Known limitations

- **Grayscale only** — color content is dithered
- **~0.5–1 FPS** — e-ink physics, not a software problem
- **No audio** — Kindle has no speakers; audio is intentionally not implemented
- **macOS 12–14 tested** — AirPlay protocol may change in future macOS releases
- **No AirPlay 2 multiroom** — screen mirroring only
- **Two ABI builds required** — Amazon switched from soft- to hard-float ABI at firmware 5.16.3; sf and hf builds are not interchangeable (see [Install](#install))
- **`mxcfb` EPDC v2 only** — tested on Paperwhite 12th Gen; behaviour on other devices is unknown

---

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) — AirPlay receiver library (git submodule)
- [RPiPlay](https://github.com/FD-/RPiPlay) — original open-source AirPlay receiver
- [Unofficial AirPlay spec](https://nto.github.io/AirPlay.html)

---

## License

UxPlay is GPLv3, so this project is therefore also GPLv3 because it wraps UXPlay.
