# crt-bridge emitter @VERSION@

Based on RetroArch @BASE_VERSION@. It is not affiliated with, or endorsed by, the libretro project. This is a pre-release build.

## Before you connect a CRT

crt-bridge sends a picture over the network to another machine, the receiver, which turns it into a signal for a monitor. The emitter cannot see that monitor and protects none. Read this section before you plug a tube in.

### Tested

This is the chain the pre-release was tried on, and nothing else.

- Emitter: Windows 11, with the `vulkan` and `glcore` video drivers.
- Receiver: a PC with an AMD Radeon HD 7750 graphics card, under the Linux `radeon` driver, wired by Ethernet, driving a JVC DT-V1710CG tube. This receiver is not part of this release.
- Software receiver (the crt-bridge client, a libretro core that shows the picture on a second computer instead of a tube): on Windows and on macOS. The Mac was also tried over Wi-Fi, with Padded Session turned on.
- A MiSTer FPGA, a SuperStation One running the Groovy core, through its composite and S-Video outputs to the same JVC tube, over Wi-Fi. Picture and gamepad work with the original Groovy core; sound needed a modified core, which is not published.

### Expected but not tested

- A MiSTer's RGB output, other MiSTer boards, and a MiSTer on a wired link. Never turn on Receiver 31 kHz or Padded Session (a Wi-Fi option) towards a MiSTer.
- The Linux emitter towards a CRT receiver. The Linux kit has only been run towards the software receiver.

### Not supported

- A master receiver reached over Wi-Fi. The master is the receiver that sets the clock: wire it. Extra receivers can use Wi-Fi.
- The `d3d12` video driver. It has never been checked with any core.
- The emitter on macOS. Only the software receiver runs there.

### CRT safety

- The emitter protects no tube. What a tube receives is decided by the receiver machine and by your settings, and a signal outside what the tube accepts can damage it. Find out what yours takes before you connect it.
- Many consumer CRT TVs take 15 kHz only. A 480p picture needs 31 kHz. Receiver 31 kHz is Off by default: turn it on only if the master receiver's tube takes 31 kHz.
- On the receiver used for testing, the PC's own start-up screen (drawn by its firmware, before any crt-bridge software runs) is a 31 kHz signal. Do not leave a tube that cannot take 31 kHz connected while the receiver machine starts.

## What it does

crt-bridge emitter sends every frame, at the running core's native resolution, over the
network to a 15 kHz CRT receiver -- or to another computer -- using the Groovy protocol. It
is a RetroArch build with one added recording driver; everything else about RetroArch is
unchanged.

## Quick start

1. Unpack this archive.
2. Put your own libretro cores in the `cores/` folder. **No core is included** -- get them
   from wherever you already get RetroArch cores.
3. Launch it: `start-emitter.cmd` on Windows, `./start-emitter.sh` on Linux. Always use one
   of these two, not the binary directly from another folder -- they switch into this folder
   first, which is what keeps everything below inside it.
4. Tell it where your receiver is. Open the menu (F1 on a keyboard by default, or hold Down + Y + L + R
   together on a gamepad) and go to Settings > Recording > Groovy Master Address. Type the
   receiver's address as ip or ip:port, for example `192.0.2.20:32100` (the port is 32100 when
   omitted). It is saved when you quit. The same value can be set in a text editor before the
   first launch: open `crt-bridge.cfg` and put it in `video_record_config`.
5. Load a core, then a game, from the menu (Load Core, then Load Content). If you set the
   address after loading the game, close it (Close Content) and load it again.

There is no default address. Until you set one, nothing is sent, and loading a game shows a
message on the PC screen that says where to set it.

Everything RetroArch writes (saves, states, screenshots, logs, playlists, the settings saved
by the menu) stays inside this folder.

## Using a MiSTer as the receiver

A MiSTer FPGA with its Groovy core can be the receiver instead of a PC. On the emitter, nothing
changes from the Quick start: set Groovy Master Address to the MiSTer's address.

**What the emitter does by itself.** When recording starts, it asks the receiver what it is. A
MiSTer core gets the original Groovy protocol: every picture at the core's own resolution, the
line held at the standard 15,734 Hz, or 15,625 Hz for a 50 Hz game (a composite encoder takes
its colour from the line rate),
and one field per frame in 480i. A crt-bridge receiver keeps its own settings. The log says
which receiver was found.

**On the MiSTer.**

- Load the Groovy core. A wired network is better: the Wi-Fi of the tested MiSTer dropped once in
  a long session, which froze the picture for about ten seconds.
- In the Groovy core's menu, set Joysticks to On, or the gamepad is not sent back.
- The Groovy core shows ten generic buttons. Map Start, Select, L and R in the MiSTer's own
  controller menu: before that, only the directions and a few buttons reach the emulator.
- Use a USB gamepad on the MiSTer.
- To drive a 15 kHz CRT, set the MiSTer's video output for a CRT. On a SuperStation One, through
  composite or S-Video (the same settings serve both, and both outputs can be connected at once):
  System menu, Video, CRT mode; DIP switch 3 up; and `vga_mode=svideo` in `MiSTer.ini`. With the
  factory settings its composite output is black and white.

**Limits.**

- Sound. The original Groovy core receives sound in small packets that a Wi-Fi link drops, and the
  packets that follow are then misread. Towards that core the emitter sends no sound.
- 480i. The tested MiSTer showed doubled frames now and then in 480i, over Wi-Fi. Not explained yet.

## Factory settings

These are what you get out of the box. All of them except the video driver are under
Settings > Recording.

- Video driver: `vulkan` on Windows, `sdl2` on Linux (`video_driver` in `crt-bridge.cfg`).
- Supersampling reduction: Auto.
- Receiver 31 kHz: Off.
- Padded Session: Off.

### Receiver 31 kHz

Off by default, and it applies when recording starts. Turn it on only if the master receiver's
tube takes 31 kHz: a picture taller than 280 lines is then sent as progressive 480p instead of
interlaced 480i. Never turn it on towards a MiSTer.

### Supersampling reduction

Auto by default. It reduces a picture the core rendered at a multiple of its native size, by the
smallest factor that fits what the receiver takes, and leaves a native picture alone; a line on
the PC screen says what was done. A Nintendo 64 core rendering at 4x ends at 480 lines under Auto:
choose 4x for a 240p picture.

## Menu and assets

The menu driver is RGUI, which uses a font built into the binary. No separate assets
directory is required or included.

## Linux notes

The bundled `crt-bridge.cfg` uses the `sdl2` video driver and the `alsa` audio driver. The
Linux kit is built by the project's CI and has only been run towards the software receiver,
never towards a CRT. `vulkan` and `glcore` are available in the binary but have not been
exercised on Linux; switching is your own experiment.

This kit points its core-info folder at its own `info/` (empty by default), so any core
description files already installed system-wide (`/usr/share/libretro/info` or similar) will
not show up in the menu until you fetch your own: Online Updater > Update Core Info Files.

## Windows notes

The binaries are not code-signed. Windows can react in two ways; neither is a sign of
tampering.

- SmartScreen may warn on first launch. The usual way through is More info, then Run anyway.
- Smart App Control, if it is on, asks a Microsoft cloud service about a program first. When
  that service cannot vouch for an unsigned program, Windows blocks it, and there is no
  exception for a single application: you can turn Smart App Control off in Windows Security,
  or not run the emitter.

To check that your download is the one published, compare its SHA-256 with the lines at the
end of the release notes (https://github.com/crt-bridge/RetroArch/releases/tag/@VERSION@). In
PowerShell: `Get-FileHash -Algorithm SHA256 <archive>`.

`nvdaControllerClient64.dll`, the client library for the NVDA screen reader, is **not**
included. RetroArch loads it on demand only if present; if you need NVDA narration, drop a
copy next to the executable yourself.

If the audio device cannot be opened, the emitter paces itself at the game's own rate
instead of running as fast as it can.

## Padded session

Never turn this on towards a MiSTer FPGA: a MiSTer only understands the classic session, and a
MiSTer does not understand a padded one.

Over Wi-Fi, small command packets are the first to be dropped, and the sound travels in them. A
padded session makes those small command packets large enough to survive. It works towards the
crt-bridge client (https://github.com/crt-bridge/libretro-crt-bridge) and towards a crt-bridge
receiver.

- The main receiver: Settings > Recording > Padded Session = On, or the environment variable
  GROOVY_PAD=on (a variable that is set always wins over the menu, and the emitter says so on
  screen). It applies when recording starts.
- An additional follower: add pad=on to its entry in GROOVY_FOLLOWERS, for example
  192.0.2.30:32100,pad=on.

On a wired link it changes nothing useful; leave it Off.

## Software receiver

No CRT receiver? The crt-bridge client is a libretro core that shows this emitter's stream
on a second computer, plays its sound and sends that computer's gamepad back:
https://github.com/crt-bridge/libretro-crt-bridge. Over Wi-Fi, see Padded session above.

## Known limitations

The release page lists what is known not to work yet, in plain terms:
https://github.com/crt-bridge/RetroArch/releases/tag/@VERSION@

## Source code

Fork: https://github.com/crt-bridge/RetroArch/tree/@FORK_COMMIT@
Bundled libgm: https://github.com/crt-bridge/libgm/tree/@LIBGM_COMMIT@

## Licenses

See the `LICENSES/` folder for the full text of every license covered by this build;
`LICENSES/NOTICE.txt` names every third-party component and its license.
