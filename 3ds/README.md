# GBA DS for the New Nintendo 3DS

This port runs GBA DS (the game on one screen, its own map, HUD or logo on the other) on the 3DS's
two screens: the game on the top screen, the second screen and the touch screen on the bottom one.
It builds on ratherDashing's 3DS port ([ratherDashing/gba-ds](https://github.com/ratherDashing/gba-ds),
branch `3ds`), whose 3DS layer it keeps.

It's an alpha, tested in the Azahar emulator and not yet on a console with this core (see the end).

It is the same app: `src/` (the game list, the menus, the per-game profiles, map captures on a hidden
copy of the game) builds for the 3DS with `#ifdef __3DS__` changes in six of its files, on the same libmgba as
every other system. What differs is underneath:

| | other systems | 3DS |
|---|---|---|
| emulator | libmgba (`src/emu.c`) | ratherDashing's armGBA with its ARM recompiler (`emu/emu_armgba.c`), the default; `CORE=mgba` builds on libmgba instead (`mgba/build-libmgba.sh`) |
| screens, input, sound | SDL2 | `ctr/sdl_ctr.c`: the part of SDL2 the app uses, on the GPU (citro2d), hid and ndsp |
| threads | pthreads | `ctr/host.c`: map captures and logos on the New 3DS's fourth core, the Frame Buffer option's emulation thread on its third |

## Building

No devkitPro needed on the machine: the builds run in the `devkitpro/devkitarm` Docker image.

    make -C 3ds 3ds      # 3ds/ctr/gba-ds.3dsx (the first time, libmgba into deps/mgba-3ds)
    make -C 3ds cia      # 3ds/ctr/gba-ds.cia (makerom and bannertool in ~/3ds-tools/bin)
    make -C 3ds libmgba  # libmgba again, after scripts/mgba-options.sh changes

makerom comes from [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR/releases) (its Ubuntu
build runs in an `ubuntu:24.04` container), bannertool from
[diasurgical/bannertool](https://github.com/diasurgical/bannertool/releases).

libmgba is the commit and options in `scripts/mgba-options.sh`, built with mGBA's own 3DS toolchain
file and these changes (`mgba/build-libmgba.sh`):

- 32-bit pixels, as on every other system (mGBA's own 3DS port uses 16-bit color).
- No fixed ROM buffer (several copies of a game run at once), and not the start-up code that
  allocates it (`ctru-heap.c`, 32 MiB).
- `mgba/rom-copy-size.patch`: a copy of the game that writes to its ROM (a profile's patch, a
  cartridge's clock or rumble) gets a copy the ROM's size, not 32 MiB.
- `mgba/memory-map-hook.patch`: mGBA's large blocks come from GBA DS's allocator (`ctr/emu_ctr.c`,
  see Memory).
- `mgba/gba-fast-memory.patch`, `gba-ldm-stm.patch`, `gba-dma-batch.patch`, `color-mix.patch`: fewer
  instructions for the same result (short paths for loads, stores and jumps, LDM/STM over the set
  bits, a DMA's units without the event list, a cheaper alpha blend). Pictures, RAM and states are
  bit-identical to unpatched mGBA's.
- `mgba/gba-stall-closed-form.patch`, `gba-jump-inline.patch`, `gba-audio-fast.patch`,
  `gba-audio-silent.patch`: the same kind of change again (the ROM prefetch model's loop as arithmetic,
  the usual jump checked inside the jumping instruction, no memset, division or table copy per sound
  event, the PSG's output made once per run of equal samples). Same pictures, RAM, states
  and sound samples as before.
- `mgba/video-wake.patch`: `video_wake` in `gba-ds.cfg` (0, 1, 3, 7, 15, 31 or 63; 15 as before)
  is how often mGBA's drawing thread is woken, once per N+1 lines.
- `MINIMAL_CORE` taken out of the `flags.h` it writes: the library is built without it, and the
  app's view of mGBA's structs has to match.
- No `.7z` support (`USE_LZMA=OFF`).

The CIA takes armGBA-3DS's `.rsf` (the 80 MB mode, among others), so `make cia` needs
`git submodule update --init third_party/armgba`, as `CORE=armgba` does.

## On the console

- `gba-ds.3dsx` in `sdmc:/3ds/`, or install `gba-ds.cia`. A New 3DS or New
  2DS (the recompiler's and the hidden copy's threads use its extra cores; an
  Old 3DS starts it, far too slowly to play).
- Games (`.gba`, or one in a `.zip`) in `sdmc:/roms/gba/`. Another
  folder: `rom_dir` in `sdmc:/3ds/gba-ds/gba-ds.cfg`, which is written with
  the 3DS's defaults the first time the app runs.
- Only games GBA DS has second-screen support for are listed, as everywhere;
  `show_all = 1` in `gba-ds.cfg` lists every game. A game is known by the
  four-letter code in its header: a translation or romhack that kept the
  original's code is listed (and treated as that release), one with a code of
  its own only with `show_all = 1`, on one screen.
- The game is drawn on a thread of its own on the third core, one frame behind the game, so the game never
  waits for the drawing, and that thread also widens each picture for the screen (`armgba_picture_thread = 1`
  and `render_workers = 1`, the defaults: on a New 2DS XL they beat two drawing threads without it in 46 of
  53 games' slowest stretches). `render_workers = 2` in `gba-ds.cfg` draws every other line on the fourth
  core too, as armGBA-3DS does; `armgba_picture_thread = 0` widens on the game thread. The fourth core is
  also the hidden copy's (map captures).
- The picture's copy to its GPU texture is queued in the frame being drawn and runs while the next frame emulates
  (`async_upload = 1`, the default; 0 waits for it in the frame, as before). With one drawing thread the picture
  thread can also write the picture in the GPU's byte order into linear memory and flush it, so the game thread does
  no byte swap and no cache flush (`gpu_twin = 1`, off by default: on a New 2DS XL its extra pass slowed the game
  thread more than it saved; it needs a start-up self-test to pass, and the log says "GPU twin self-test"). Both
  draw the same pictures at the same time.
- `raw_picture = 1` (the default on the 3DS; 0 for the byte swap: on a New 2DS XL it took 0.5 to 0.8 ms a frame off Crash
  Nitro Kart, GT Advance 3 and Tactics Ogre in the same program's A/B) has no second copy at all: with one drawing
  thread the picture thread widens each picture straight into linear memory with its rows 256 words apart, as the
  texture's are, writes it back itself, and one transfer of the copy engine tiles it as it is, so the game thread does
  no byte swap and no cache flush (what reads the picture as rows of 240 words, `emu_fb()`, gets a copy of it made
  once a picture; the pixel readers, whole-picture copies and compares (`emu_fb_copy`, `emu_fb_same`) and the HUD's puts use the real rows). The picture's channels then come out in the
  wrong order, and the GPU puts them right when it draws (three texture combiner stages: each channel times an
  all-or-nothing mask, added up; a mix of two channels was off by one on the console, this is exact there). The first game
  opened with the option on runs a self-test that draws pictures both ways, compared word for word, and falls back to
  the byte swap if they differ (the log and the bench say "raw picture self-test"). `raw_picture = 2` keeps rows of 240
  and copies in two passes instead (a linear copy that leaves a gap after each row, then the usual transfer).
  `raw_check = 1` and `tex_check = 1` are tests: each picture drawn both ways offscreen and compared, each texture
  read back; their verdicts are bench lines.
- `SDL_GetTicks` on the 3DS counts the system tick, not `osGetTime`: that clock steps (the log says "clock:
  osGetTime moved ..."), and a step back looked like a pause of over two seconds to the game loop, which rebuilt
  every picture (one 200-370 ms frame, seen as the bench's "pre stall" in "events and buttons").
- The HUD pictures on the bottom screen (Mario Kart, Crash Nitro Kart, Sonic Advance, V-Rally 3 and the
  like) can be a backdrop with the HUD's pieces laid over it by the GPU, so a frame sends only the rows
  that changed (`hud_layers = 1`, the default on the 3DS); `hud_layers = 0` makes it a third at a time over
  three frames, as ratherDashing's build does (see Speed).
- Sound needs the DSP firmware dump (`sdmc:/3ds/dspfirm.cdc`), as all 3DS
  homebrew does.
- Saves are beside the ROMs (`.sav`), save states and settings in
  `sdmc:/3ds/gba-ds/states/`, the log in `sdmc:/3ds/gba-ds/log.txt` (the run
  before it: `log-prev.txt`).
  Save states are mGBA's, as on every other system: states made with the desktop build load on the 3DS
  (the other way hasn't been tried).

Buttons: A, B, L, R, START, SELECT and the D-pad (or the Circle Pad) are the GBA's. **ZL** is GBA DS's
menu button (its HOME: the pause menu, and held with another button, the hotkeys: ZL + R saves a
state, ZL + L loads it, ZL + X the quick menu, ZL + Y swaps the screens, ZL + B changes the shader,
ZL + START closes the game). **ZR** is fast-forward, without sound (`ff_mute = 0` in `gba-ds.cfg`
plays it sped up). The menus also work by touch.

The first time a folder of games is read, each file is opened once for its game code ("Loading
games..." on the bottom screen: a few seconds per hundred files); the codes are kept in
`states/codes.cache`, and later starts list the folder at once. Each supported game's logo is
made the first time, in the background, by running its title screen: the list is slower until those
are done.

## Logs over Wi-Fi

A diagnosis build (never a release) POSTs its logs to a machine on the local network, so the SD
card stays in.

    python3 3ds/tools/logserver.py                       # listens on port 8377, writes build/logs/
    touch 3ds/ctr/host.c
    make -C 3ds 3ds LOG_URL=http://<that machine's address>:8377/log

It sends the end of the last run's log (up to 768 KiB) when the app starts, the log so far each time
the game list comes up, and this run's when the app closes. Build without `LOG_URL` again
(after `touch 3ds/ctr/host.c`) for anything given to someone else.

## What isn't there

- Rewind (thirty seconds of states are more than the console's memory), RetroAchievements, the
  updater, `.7z` archives, the bug report's ten seconds of history.
- The game list's notes on romhacks and modified ROMs (the checksum means reading each game whole).
- Map screens that move keep 4 frames, not 16.
- Screenshots of the screens as drawn (the GPU's picture isn't read back).
- The pinball games' table over both screens (Pokémon Pinball, Sonic Pinball
  Party, Muppet Pinball Mayhem): it runs up to three copies of the game a frame,
  far beyond a New 3DS (0.0.1-alpha ran out of memory opening it and froze).
  They play on the top screen with their logo on the bottom one, whatever
  `pinball_live` says.

## Memory

A New 3DS gives homebrew 64 MiB: about 46 MiB of heap here, after 12 MiB of
linear memory for the GPU and the sound. The game's ROM, the recompiler's
code (what is left after the ROM and 15 MiB kept for the rest, up to 16 MiB:
about 13 MiB with Mario Kart; 22 MiB kept for a game with a hidden copy:
about 8 MiB with Yoshi's Island; up to 24 MiB when the heap has 8 MiB to
spare beyond that, as a console with 84 MiB free has for a game of up to 20 MiB), the
hidden copy and its code (up to 4 MiB) share it. When the heap runs low, each machine's recompiler empties its own
code memory (`jitHostMemoryLow`, counted per thread). The emulator tests run
with the same heap, not the 124 MiB an emulator would give
(`make EMU_HEAP_KIB=86700` gives Azahar the 84 MiB free a New 3DS console has at a job's start, for
code memory tests).

A game's ROM is read once and shared by every copy of the game (the game, the hidden copy, a logo's),
and kept at the top of the heap's address range with the heap's end brought down below it, so even a
32 MiB game finds one stretch of memory. A plain ROM's room is set aside before the game's machine and code memory
are made, and the game's code memory goes to the top as well, under the ROM, when the break leaves it room: the
heap's peak then holds no 16-24 MiB of code, so a small block a game leaves behind (it is made at the peak) can't hold the
break above the room the next 32 MiB ROM needs (a bench session of 54 games left MOTHER 3 no room on a 62 MiB heap before
this). The hidden copy's scratch states go back with the game for the same reason. mGBA's own large blocks (a ROM copy for a copy of the game
that writes to it) come from there too (`mgba/memory-map-hook.patch`). MOTHER 3 starts in the 64 MB mode
with about 9 MiB of the heap free; the CIA asks for the 80 MB mode. The Azahar emulator gives 124 MiB;
`host.c` keeps the heap to the console's size there, so that what is tested is what the console has.

## What has and hasn't been tested

Every supported game (2026-11-16), on the desktop build drawn as the 3DS draws
(`3ds/test/dualrun.py --runs mgba,armgba-3ds`: two threads, a frame late, the
HUD split one frame in five) against GBA DS on libmgba: each game's script in
`3ds/test/play/` plays from power-on to its second screen (the map, status
screen or HUD its rules describe; Dragon Ball Z: The Legacy of Goku II's
Scouter through the game's Debug menu, `--cheats 3ds/test/cheats`; Mother 3 run
with the English translation's ROM by path), and the second screens match.
Fire Emblem: Fuuin no Tsurugi, new in GBA DS since, hasn't a script yet.

On a New 3DS (2026-11-16, one console): Mario Kart: Super Circuit's races
at 60 frames a second, several in a row (Peach Circuit, Boo Lake and others),
with the HUD and the course map on the bottom screen; the speed work is below.

On a New 3DS (2026-11-12, one console): the game list with 124 files,
Castlevania: Circle of the Moon at 60 frames a second with its map on the
bottom screen, save states and fast-forward. Pokémon Pinball froze on its table
with 0.0.1-alpha; 0.0.2-alpha's fix (see "What isn't there") has been checked in
Azahar, not yet on a console.

Every supported game, on the desktop build of the app on armGBA
(`3ds/sim/app.mk`) against the same app on libmgba, with
`3ds/test/dualrun.py` and a script per game in `3ds/test/play/` (2026-11-15,
GBA DS 1.5.4-dev). Most scripts get into play (a level, a race, a town or a
battle); some stop at a name entry, an intro or an attract demo (the Dragon Ball
Z games, Fire Emblem, Golden Sun: The Lost Age, Sword of Mana, Sonic Advance 1
and 3, Tony Hawk's Pro Skater 3), so their games' in-play rules weren't seen.
Each opens GBA DS's menu, saves and loads a state and swaps the screens. In all of them the app on armGBA
draws both screens exactly the same with and without its render thread, and
its second screen shows what it does on libmgba (the same captures succeed,
within a capture or two where the two emulators' runs drift a few frames
apart). Two games' second screens were wrong on armGBA and are fixed (Harry
Potter and the Prisoner of Azkaban's, Super Mario Advance's). In Azahar: the
game list, Pokémon Pinball on its table, and the menus' options.

Not tested on a console: most games' speed. Where a profile depends on exactly
how mGBA times something, armGBA may differ: the two emulators drift a few
frames apart within a minute of play (a game's timing, not its picture), so a
second screen that changes on a particular frame may change a frame or two
later. A 32 MiB game with a hidden copy hasn't been tried (there may not be the
memory for both: the game runs, the second screen may not).

In the Azahar emulator with the 3DS app on the mGBA core (2126.2, a New 3DS whose CPU it runs at 268 MHz, a third of
the console's 804), with the heap kept to the console's size:

- Each of these starts without a crash, runs its opening for 30 seconds and shows the logo the hidden
  copy made on the bottom screen: Advance Wars, Advance Wars 2, F-Zero: GP Legend, F-Zero: Maximum Velocity, Fire
  Emblem, Fire Emblem: The Sacred Stones, Golden Sun, Golden Sun: The Lost Age, A Link to the Past,
  Mario Kart: Super Circuit, Mario & Luigi: Superstar Saga, Mario vs. Donkey Kong, Metroid Fusion,
  Metroid: Zero Mission, MOTHER 3 (the English translation 1.1), Pokémon Pinball: Ruby & Sapphire,
  Super Mario Advance 1 to 4 and Wario Land 4 (the USA releases).
- From save states made with the desktop build: Mario Kart's race HUD on the bottom screen, and
  Metroid: Zero Mission's map captured on the hidden copy.
- MOTHER 3 starts in the 64 MB mode. Its in-game captures haven't been tried.

The heaviest scenes logged there take 38 to 44 ms of the emulated CPU's time a frame: Advance Wars'
opening (38.5), F-Zero: Maximum Velocity's (39.6), Fire Emblem's (39.9), Super Mario Advance's
(39.9) and Mario Kart's race with its HUD (43.4). (A map capture ran on the game's core in Azahar
and slowed the game while it lasted; on a New 3DS it has a core of its own.) At the console's clock that's about 13 to 15 ms of the 16.7 a frame has, an estimate
until a console says otherwise.

Not tested: the mGBA core on a console, an Old 3DS, odd-sized ROMs (trimmed dumps, some romhacks),
and the 32 other games GBA DS supports.

## Speed (Mario Kart)

Mario Kart: Super Circuit is the heaviest game on the 3DS: the game, its HUD
split (a second picture of every frame) and the recompiler's new code. What
gets its races to 60 frames a second on a New 3DS:

- The game drawn on two threads, one frame behind the game, so the game never
  waits for them; the HUD's picture drawn one frame in six (10 a second), and
  made and uploaded a third at a time over three frames; the pieces that go
  back on the game screen (the place, the item box) copied every frame.
- Textures uploaded by the GPU's copy engine, or by the CPU without a
  division per pixel; the HUD's two views on one texture.
- The recompiler: a warm list per game (`sdmc:/3ds/gba-ds/warm/`, the code
  the game ran, compiled in the frames' spare time next time, up to 20% of the
  code memory); at most 1.5 ms of compiling in a frame; the code memory
  emptied between races once it's 75% full (when it's full the recompiler
  empties it itself, a 100-300 ms frame). armGBA's side: no blocks compiled
  from where an interrupt returns or an event falls (but for a place returned
  to 16 times, `armgba_mid_after`), careful copies only
  for blocks events fall in often and in at most a third of the code
  memory, and a full part of a stretch's leftover room used as a stretch of
  its own; a race's code levels off at about 8 of its 13 MiB.
- The game's battery save written to the SD card on a thread of its own.
- The HUD's backdrop scaled in whole numbers (floats took 76 ms).

A race still has the odd frame of 18-22 ms here and there, in busy stretches
(about 9 ms of emulating on average, the HUD 2-4, drawing 2.5).

## Speed (games the CPU holds up)

GT Advance 3, Super Mario Advance 4, Metroid Fusion and others spend their
frame in armGBA itself. What cut it, with the same pictures, RAM and sound
(checked on the desktop build and by the bench's hashes):

- A place the CPU keeps returning to in the middle of a block (an interrupt's
  return, line after line) gets a block of its own after 16 returns, in place
  of the rest of the block being stepped on the exact CPU
  (`armgba_mid_after`; 0 steps as before; at most one such block for each
  16 KiB of code memory between flushes).
- The render log's entries reach its workers once a line, and the sound log
  takes one barrier an entry instead of three: the ARM11's barriers are slow
  (`armgba_lean_logs = 0` for the old way, to compare on a console).
- A careful copy lets more events run after the instruction they fall in, not
  inside it on the exact CPU: a sound timer's overflow that asks its FIFO's DMA
  for nothing, and any event at all when the instruction only fetches (data
  processing without a register shift, not into r15), where the DMA the event
  starts takes the bus at the next instruction's fetch as before
  (`armgba_defer = 0` for the old way, to compare on a console).
- A flag-setting multiply is followed by a check (its time depends on its
  operands), so its carry was worked out even where the next instruction sets C
  again, as almost every one in Crash Nitro Kart's races does (about 800 a
  frame, each a call through the multiplier's stages). With
  `armgba_lazy_carry = 1` (the default on the 3DS, 0 to compare) only that
  check's way out to the exact CPU works it out.
- A return into the middle of a block (an interrupt's return, or where the
  exact CPU stopped after stepping up to an event) can carry on in the block's
  careful copy, which has a way in before every instruction, instead of the
  exact CPU stepping the rest of the block: GT Advance 3 stepped about 260
  instructions a frame that way (`armgba_mid_careful = 1` where a copy is
  there, `2` also making one as an event inside the block would, `3` as 2 except
  that a place returned to again and again still gets a block of its own, which
  runs it faster than the copy; the 3DS default is 3, 0 to compare). On a New 2DS XL
  these two (at level 2; 3 timed the same) took 0.05 to 0.3 ms a frame off Crash Nitro Kart, Tactics Ogre and Super Mario Advance 2.
- With the picture drawn on threads, the machine's own renderer is read only for
  DISPCNT, its latch and what a register reads back: the copies that draw take
  their state from the log and from `renderThreadClone`. `armgba_cside` is a bit
  mask of ways the emulation thread does less for that (31 by default on the 3DS, 0 to
  compare; on a New 2DS XL bits 1 and 2 took about 0.7 ms a frame off Crash Nitro Kart and
  GT Advance 3, 4 and 8 another 0.2 off GT Advance 3, and 16 0.2 more there): 1 keeps only those in the machine's renderer (a register write
  no longer updates the renderer's backgrounds, windows, blends and per-line
  caches, and a line start no longer runs its affine step); 2 makes a 32-bit
  write to two video registers one log entry, and with 1 writes the render log's
  register entries straight into the ring while it has room and takes a DMA run's
  writes to video registers without `ioWrite` and `ioWrite32`; 4 (with 1 and 2) keeps the register writes of one
  DMA run to video registers in a batch and logs them as one entry (GT Advance 3
  moves them 16 bits at a time); 8 makes the exact CPU's decode clear an
  instruction with stores rather than a call to `memset`. Crash Nitro Kart and
  GT Advance 3 write about 1300 video registers a frame from their H-blank DMA.
  16 is for interrupts: the changes of the interrupt controller that only clear
  a flag (what acknowledging an interrupt does to IE & IF and then to the CPU's
  IRQ line, a cycle or two after the write) wait in the event list as before,
  but compiled code no longer stops for them, and whatever looks at what they
  change (a write to IE, IF or IME, the IRQ line, a saved state) runs the ones
  due first; GT Advance 3 acknowledges about 220 a frame.
- `armgba_compile_budget` (ms of compiling a frame of the game may take before code met for the first times is put off,
  1.5 as built; 1000 never puts any off) is a knob for comparing on a console: careful copies no longer wait for it.
- The interrupt pipeline's events and a halfword store to an I/O register
  take shorter ways through the core.
- A compile no longer clears the 21 KB of state it kept for the worst case, only
  what it uses: Golden Sun 2's second of Sol Sanctum compiles about a thousand
  blocks at once. `armgba_compile_after` (2 as built) is how many visits to a
  new block are stepped on the exact CPU before it compiles; a console can try
  4 to 16 where bursts like that are the slow part.
- A flush hands back every block's records in one go (they are kept in an arena)
  instead of freeing 4 to 8 pieces a block: Golden Sun 2's flush of 8,700 blocks
  took 28.6 ms in Azahar and takes 4.7. The game's code memory is 24 MiB, not 16, when
  the heap has the room: Golden Sun 2 and Sword of Mana filled 16 MiB in their long
  routes, and each flush was a long frame and a few seconds of recompiling. In a bench
  job the `armgba_stats = N` lines (what compiled, why and when it flushed, the code
  and heap used) go to the job's results, not the log, which drops writes while the
  card is busy.
- `make PGO=use` builds armGBA's files from a profile (`3ds/emu/pgo/`, made
  with `PGO=gen` and `3ds/emu/pgo-train.py`; both builds need `BUILD=build/gen`,
  emptied first: GCC names a static function's profile by its object's path):
  about 6% less to emulate in Azahar. A function whose code or line number
  changed since the profile was made loses its profile (`PGO_QUIET=` lists
  them): redo it after armGBA changes. `HOTCOLD=1` also keeps the profile's
  hot code together.

## Speed (Yoshi's Island)

Yoshi's Island's status screen is captured on the hidden copy every 3
seconds on the 3DS (`map_interval` 180; every second elsewhere): every
second, the captures kept that copy busy most of the time and slowed the
game. Its events fall inside its code so often that careful copies took half
of what it compiled, and the code memory filled every 20-40 s; with armGBA
0.0.6's changes, 4 minutes ran with no frame over 100 ms after starting.

Pictures go to the GPU's copy engine through two buffers in turn
(`SDL_UpdateTexture`): with one, the game's picture was written over the
second screen's while the copy was still reading it (a torn status screen on
the console; Azahar's copies finish at once).

## armGBA

`third_party/armgba` is armGBA-3DS: armGBA's main with the 3DS port on top
(ratherDashing/armGBA). Every build that compiles it copies its `src/` into the build folder and applies
`emu/armgba-patches/*.patch` there (`emu/armgba.mk`), so the submodule stays at its public commit and clean.
What GBA DS uses of it that other frontends don't:

- `struct RenderHooks` (`src/video/render.h`): layers and sprites left out
  of the picture, a callback before each line, and a second picture drawn by
  a renderer of its own beside the first (the HUD split). Kept beside the
  machine, not in it: states are unchanged. The second picture can be drawn
  one frame in `secondEvery` (`secondCount` says when there's a new one).
- `videoDrawSkipped` per thread (the hidden copy runs frames it doesn't draw
  on a thread of its own).

`ctr/jitmem.c` is armGBA's `platform/3ds/source/jitmem.c` with code memory
for several machines at once (the game: up to 16 MiB; each copy: up to 4).

## Files

    mgba/              libmgba for the 3DS: build-libmgba.sh and its two patches
    ctr/               the 3DS host: SDL's part (sdl/SDL.h, sdl_ctr.c), host.c, emu_ctr.c (shared ROMs), the CIA's parts
    emu/emu_armgba.c   src/emu.h on armGBA (CORE=armgba), with ctr/jitmem.c
    sim/               the desktop test build of the armGBA core
    test/              the Azahar test's script and SD card files
