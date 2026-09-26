# SouthparkDS

Homebrew DSi app that downloads South Park episodes (encoded as FastVideoDS
`.fv` files) over plain HTTP and plays them from the DSi's SD card.

## Layout on the machine

```
SouthparkDS.nds          <- the app (grab the latest from a "Release", or build: copy to SD, run via TWiLight Menu++)
gfx/icon.bmp             <- 32x32 paletted banner icon (palette index 0 = transparent)
gfx/mkicon.py            <- PNG -> icon.bmp converter (pure Python, no PIL)
gfx/patchicon.py         <- re-applies icon.bmp to a built .nds (ndstool drops it)
source/                  <- the homebrew source
  config.h               <- compile-time defaults (host, port, URL prefix)
  http.c/.h              <- dswifi + blocking HTTP/1.1 client (plain HTTP only)
  index.c/.h             <- small JSON parser for the episode index
  settings.c/.h          <- runtime server URL, saved to SouthPark/config.txt
  main.c                 <- UI, keyboard, download, /health check, clear-cache
Makefile                 <- devkitARM / calico build (see "Building")
tools/release.sh         <- build + icon + tag + GitHub release + DSi upload
```

Files get saved to the **SD card root** (where `SouthPark/config.txt` and the
`SouthPark/` dir live), named `<season>_<episode>.fv` (e.g. `3_2.fv`).

## Server setup

The DSi's socket stack (dswifi + calico) has **no TLS**, so everything must be
served over **plain HTTP**. Host the following on any web server (the bundled
`C:\Users\camer\SouthparkDS-server` is one such server):

```
<prefix>/index.json       episode index (JSON, see below)
<prefix>/1_1.fv            episodes, named <season>_<episode>.fv
<prefix>/1_2.fv
...
<prefix>/health            must answer "ok" (checked on every boot)
<prefix>/FastVideoDS.nds   optional: the player (Downloadable from the menu)
```

`source/config.h` holds the build-time defaults (used when there's no SD config):

```c
#define HTTP_HOST   "10.0.0.39"      /* no scheme, no port */
#define HTTP_PORT   8080
#define HTTP_PREFIX "/"              /* must start with '/' */
#define INDEX_FILE  "index.json"
```

### Where the DSi gets the server address

At startup the app reads `SouthPark/config.txt` from the SD card:

```
server=abcd1234.ngrok-free.app      (port 80 is assumed)
server=10.0.0.39:8080                (explicit port)
```

If the file is missing, the `config.h` defaults are used. You can edit it on
your PC or, easier, from the DSi: **Server URL...** in the in-app menu opens an
on-screen keyboard (A=type, B=del, X=clear, L/R=row, `<-`/`->`=cursor,
START=save). Schemes (`http://`) and trailing `/path` are stripped automatically.

### Boot behaviour

On every boot the app:
1. connects to WiFi (WFC profile),
2. checks `<prefix>health` on the configured server,
3. fetches `<prefix>index.json`.

If the health check (or the index) fails you get a screen with:
**A=change URL, B=retry, START=quit**.

### index.json format

```json
{
  "seasons": [
    { "season": 1, "episodes": [1, 2, 3] },
    { "season": 2, "episodes": [1, 2] }
  ]
}
```

Seasons appear in the order given; episodes within a season likewise.

## Playing on the DSi

1. Put `SouthparkDS.nds` on the SD card, plus TWiLight Menu++ and the
   FastVideoDS player. If you only have the app: open the **Get FastVideoDS
   player** menu item on the DSi and it downloads `FastVideoDS.nds` from the
   server to `SouthPark/`.
2. Configure WiFi in the **DSi System Settings** under
   "Internet -> Nintendo WiFi Connection Settings" (the app uses the stored
   WFC/dsiWiFi profile).
3. Run `SouthparkDS.nds` from TWiLight Menu++.
4. Pick a season, pick an episode; the app downloads it to the SD card root.
5. Press START to return to the TWiLight Menu++ and open the
   `<season>_<episode>.fv` file there - TWiLight routes `.fv`
   to FastVideoDS Player automatically.

## Building

Requires devkitPro with the nds/calico toolchain on **Windows** (this was
tested from WSL by invoking the installed Windows toolchain):

```
C:\Users\camer\SouthparkDS> make
```

or, from WSL, call the Windows make directly:

```
DEVKITPRO='C:\devkitPro' DEVKITARM='C:\devkitPro\devkitARM' \
PATH='C:\devkitPro\devkitARM\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin' \
/mnt/c/devkitPro/msys2/usr/bin/make.exe
```

Produces `SouthparkDS.nds`.

Note: the banner icon (`gfx/icon.bmp`) is silently dropped by cross-compiling
(`ndstool`) builds, so a plain `make` yields the default devkitPro banner.
Re-apply the real icon after any clean build:

```
python gfx/patchicon.py SouthparkDS.nds gfx/icon.bmp SouthparkDS.nds
```

## Releases

Prebuilt `.nds` files are attached to GitHub releases
(`github.com/Qwerty9957/southparkds-client/releases`) - grab the latest one,
drop it on the SD card, no build needed.

Publishing a new version (from WSL, builds + icons + tags + GitHub release +
DTG upload in one go):

```
tools/release.sh              # auto-bumps the patch version
tools/release.sh v1.2.0      # or pick a version explicitly
```

`Edit the icon` in `gfx/icon.bmp` (8-bit paletted 32x32, palette index 0 =
transparent) and rebuild; regenerate it from a PNG with `gfx/mkicon.py`.

## Notes / limitations

- HTTP only, no redirects/chunks/TLS. Keep the server simple.
- The HTTP client sends `ngrok-skip-browser-warning: true` and treats a
  `text/html` answer as an error (so an ngrok interstitial shows a clear
  "no reach / HTML page" message instead of being parsed).
- One worker socket, downloads are serial and are shown with progress dots
  on the top screen; a 5-15 MB `.fv` takes a while, that is normal.
- "Clear cache" only deletes files matching the current index.
- The 32x32 icon is a placeholder; replace `gfx/icon.bmp` with your own
  8-bit paletted bitmap and rebuild (see "Building").
- On this DSi the ARM7 hinge auto-sleep wakes instantly; the app drives lid
  blanking itself (30-frame debounce + latch until the hinge reopens).