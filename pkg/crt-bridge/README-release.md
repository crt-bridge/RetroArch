# crt-bridge emitter @VERSION@

Based on RetroArch @BASE_VERSION@. It is not affiliated with, or endorsed by, the libretro project. This is a pre-release build.

## What it does

crt-bridge emitter sends every frame, at the running core's native resolution, over the
network to a 15 kHz CRT receiver -- or to another computer -- using the Groovy protocol. It
is a RetroArch build with one added recording driver; everything else about RetroArch is
unchanged.

## Quick start

1. Unpack this archive.
2. Put your own libretro cores in the `cores/` folder. **No core is included** -- get them
   from wherever you already get RetroArch cores.
3. Open `crt-bridge.cfg` in a text editor and set `video_record_config` to your receiver's
   address and port (for example `192.0.2.20:32100`).
4. Launch it: `start-emitter.cmd` on Windows, `./start-emitter.sh` on Linux.
5. Open the in-game menu from a gamepad by holding Down + Y + L + R together.

## Menu and assets

The menu driver is RGUI, which uses a font built into the binary. No separate assets
directory is required or included.

## Linux notes

The bundled `crt-bridge.cfg` uses the `sdl2` video driver and the `alsa` audio driver --
the only Linux path tried against a receiver so far, over a software receiver. `vulkan` and
`glcore` are available in the binary but have not been exercised on Linux; switching is your
own experiment.

## Windows notes

The binary is not code-signed: Windows SmartScreen may warn on first launch. This is
expected for an unsigned hobby build, not a sign of tampering.

`nvdaControllerClient64.dll`, the client library for the NVDA screen reader, is **not**
included. RetroArch loads it on demand only if present; if you need NVDA narration, drop a
copy next to the executable yourself.

## Source code

Fork: https://github.com/crt-bridge/RetroArch/tree/@FORK_COMMIT@
Bundled libgm: https://github.com/crt-bridge/libgm/tree/@LIBGM_COMMIT@

## Licenses

See the `LICENSES/` folder for the full text of every license covered by this build.
