# Osmos — Nintendo Switch port (GLSurfaceView / JNI wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
Osmos on Switch homebrew. It contains no game code and no game assets — it
loads the game's own library and recreates, natively, the thin Android/JNI
layer the engine expects.
 
## Install & run
 
You need files from Osmos v2.9.0.
 
Put the `.nro` in any folder under `sdmc:/switch/` and place your game files
next to it — the loader finds its folder at runtime, so the name is up to you:
 
```
sdmc:/switch/osmos
├── osmos_nx.nro
├── libosmos.so                              <- from your APK: lib/arm64-v8a/
├── cursor.png                               <- optional
└── assets/                                  <- the APK's assets/, whole
```
 
Launch via title override (hold R while starting an installed game) or a
forwarder.
 
Optionally drop a `cursor.png` (up to 64×64, transparency respected) in the
same folder to replace the on-screen cursor with your own.
 
## Controls
 
| Input | Action |
|---|---|
| Touchscreen | Direct multi-touch (handheld) |
| **+** | Toggle the on-screen cursor |
| **−** | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| **L / R** | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| **A / ZR / ZL** | Tap / confirm (ZL and ZR let you play one-handed) |
| **B** | Android Back |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
 
The cursor is on by default when docked and off in handheld; **+** overrides
either way. A USB mouse works in both modes: move to control the cursor,
left-click to tap, and use the scroll wheel to change sensitivity — gyro turns
itself off while a mouse is connected. Your stick, mouse and gyro sensitivities
are remembered in `pointer.cfg` automatically after in-game adjustment.
 
## Settings
 
`config.txt` is written next to the `.nro` on first launch, with the options
documented inline. A file from an older build gains any new settings
automatically, keeping the values already in it.
 
```
language        auto     # auto, or en de es fr it ja ko pt ru zh-Hans iu
light_mode      locked   # "owned" unlocks it if you bought it on Google Play
pan_sensitivity 6.0      # strength of the one-finger pan time control
pan_deadzone    64       # finger travel before a pan starts, in panel pixels
```
 
The game renders at 1080p in both handheld and docked — it is light on the GPU,
and a fixed render size means the UI never has to be rebuilt on a dock.
 
`light_mode` exists because Osmos sells that mode through Google Play, which
does not exist here, so the game can neither sell it nor look up what you own.
 
The two pan settings tune the one-finger left/right time control. Osmos sets
its thresholds as a fixed fraction of screen width, which is the same on every
phone it shipped on — but this panel is physically about twice as wide, so the
same fraction means twice the finger travel. The defaults restore the feel.
 
## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib
 
export DEVKITPRO=/opt/devkitpro
make                        # -> osmos_nx.nro
```
 
`make check` runs the host-side audits — unresolved symbols, import coverage,
JNI vtable slots, stub return polarity, and files left behind by an older
version.
 
## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `nx_pointer`,
diagnostics) derives from the open-source Switch `.so`-loader lineage — Andy
Nguyen, fgsfds and ChanseyIsTheBest, building on TheOfficialFloW's Vita/Switch
loader tradition — reaching this project via the Killer Bean Unleashed and
Sonic Jump ports. All MIT-licensed. Thanks to everyone in that lineage for
making this approach possible.
 
Osmos is by Hemisphere Games. You need to own it; nothing of theirs ships here.
 
