Shatter %VERSION% for Mirror's Edge
=====================================

A CPU replacement for the game's PhysX effects (glass shards, debris, smoke, steam, cloth),
multithreaded, for any GPU, on Windows or Linux (Wine/Proton).

Install
-------
Easiest: run ShatterSetup.exe (from the installer download) and pick MirrorsEdge.exe.
It keeps a backup of the original file as PhysXLoader.dll.original.
Its Game settings button (after installing, or when you run it again) can raise the game's
62 fps cap and skip the intro movie, in Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini
(backed up first as TdEngine.ini.shatter-backup).
Optional, off by default because it changes player movement: a fix for steering on steep
slides (chapter 1 building slide), which the game loses above 62 fps. Game settings checkbox, or
slide_steering_fix = on in shatter.ini. GOG 1.0.1.0 game build only.

By hand:
1. Open the game folder, then Binaries (the folder with MirrorsEdge.exe).
2. Optional: copy the original PhysXLoader.dll somewhere safe, or rename the copy to
   PhysXLoader.dll.original.
3. Copy PhysXLoader.dll from this zip into Binaries and overwrite the original.
4. In the game, make sure PhysX is on in the graphics options.

Settings (optional)
-------------------
shatter.ini goes next to PhysXLoader.dll (every line in it starts commented out, so copying it
changes nothing until you edit it). Without it everything is at its default.
It can switch Shatter's fluids or cloth off (to compare with the original), set the
number of threads, or write a debug log for bug reports.

Uninstall
---------
Put the original PhysXLoader.dll back:
- Steam: Properties > Installed Files > Verify integrity of game files
- GOG Galaxy: Manage installation > Verify / Repair
- or rename your PhysXLoader.dll.original back to PhysXLoader.dll
  (ShatterSetup.exe can also do this: Uninstall).

License
-------
%LICENSE%
