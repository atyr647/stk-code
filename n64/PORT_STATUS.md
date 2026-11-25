# SuperTuxKart on the N64: port status

Goal: all stock content, downscaled to N64 budgets; networking off but
switchable back on; tested in ares using the build scripts from
[atyr647/Pak](https://github.com/atyr647/Pak).

## Where it stands

| Area | State |
|---|---|
| Toolchain, ares | Built with Pak's `tools/build_n64_toolchain.sh` and `tools/build_ares.sh` (GCC 16.2.0, ares with the N64 core) |
| libdragon | `preview` branch, pinned ([build_libdragon.sh](build_libdragon.sh)). Pak pins `trunk`, which has no kernel threads or OpenGL. Two patches ([patches/](patches/)): a thread start-up race that could hang the ROM in an endless exception loop, and file sizes in directory walks. |
| STK on libdragon | Headless `SERVER_ONLY` build. **In ares, a complete 4-kart Lighthouse race runs to the finish** (STK's real `main()`, 3 AI karts plus the profiling kart, every lap) and shuts down cleanly. 22 s from boot to the race; peak heap 2,661 KB of 2,895 KB. |
| Blocker | **CPU speed.** The race logic alone runs at 19 frames/s, about 3x slower than real time (see [Performance](#performance)). |
| Assets | Every stock track, kart, texture and sound is converted ([tools/stk_n64_assets.py](tools/stk_n64_assets.py)). **All 21 race tracks validated as faithful.** |
| Collision | Served from cartridge ROM through a 128 KB cache, bit-for-bit what STK would build in RAM ([details](#track-collision-from-rom)). |
| **Full game (client)** | **Boots in ares to STK's real menus** (profile creation with the on-screen keyboard, login, main menu), driven by the N64 controller, and **races run** on Snow Peak, Shifting Sands, Snow Mountain, Mini Golf, Hacienda and Fort Magma (see [Client](#client-the-full-game)). Under 1 frame/s in a race. Lighthouse and Zen Garden do not fit in RAM yet. No audio yet. |

## Building and running

```sh
# once: toolchain and ares from Pak, then libdragon preview and zlib
JOBS=4 ~/Pak/tools/build_n64_toolchain.sh /opt/pak-n64
~/Pak/tools/build_ares.sh /opt/pak-ares
N64_INST=/opt/pak-n64 n64/build_libdragon.sh
n64/build_n64_deps.sh <zlib source> /opt/n64deps

# downscale the stock assets (needs n64/tools/meshsimplify built against
# meshoptimizer v0.22, plus oggdec)
MESHSIMPLIFY=n64/tools/meshsimplify \
  python3 n64/tools/stk_n64_assets.py --assets stk-assets --out n64data

# per-track collision files and precompiled track scripts, written by the
# hosted big-endian MIPS build (same byte order and word size as the N64;
# n64/build_mips_deps.sh) under qemu, and verified as they are written
QEMU_LD_PREFIX=/usr/mips-linux-gnu n64/tools/export_collision.sh \
  "qemu-mips build-mips/bin/supertuxkart" <root whose data/ is n64data>

# build: LTO saves ~1 MB of RAM. -DSTK_N64_CONSOLE=ON adds an on-screen
# log (600 KB of heap); otherwise the log goes to ISViewer/USB only.
cmake -S . -B build-n64 -DCMAKE_TOOLCHAIN_FILE=n64/toolchain-libdragon.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DSERVER_ONLY=ON -DCHECK_ASSETS=OFF \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DCMAKE_AR=/opt/pak-n64/bin/mips64-elf-gcc-ar \
  -DCMAKE_RANLIB=/opt/pak-n64/bin/mips64-elf-gcc-ranlib
cmake --build build-n64 -j

# pack and run
STK_N64_SERVER=1 STK_N64_TRACKS=lighthouse STK_N64_KARTS=tux,gnu \
  n64/make_rom.sh build-n64 n64data stk.z64 n64/runs/profile_race_lighthouse.txt
LOG_UNTIL='Number of frames|halted|EXCEPTION|fatal' \
  n64/tools/ares_run.sh stk.z64 shot.png 1800      # log: shot.log
```

Regression tests: [tests/threads](tests/threads) (a ROM for the threading
layer; passes in ares) and [tests/collision](tests/collision) (the compact
collision mesh against stock Bullet; passes).

## Asset pipeline (all stock content)

The output is an ordinary STK data tree: same layout, XML and file names, so
the unmodified desktop game loads it. That makes the PC game the reference for
"did downscaling break anything".

- **Meshes:** decimated with meshoptimizer, which only removes triangles, so
  UVs, normals, colours and skinning weights stay exact. Each track mesh has
  three zones around the driveline:
  - the driving surface (driveline quads): at most 3 cm of error;
  - within 20 m of it (and from 8 m below to 15 m above): walls, barriers
    and every mesh karts collide with, including invisible `physics-only`
    walls: at most 25 cm;
  - beyond: at most 6 m. (This was a fraction of the track's size, 57 m on
    Cocoa Temple, and ceilings sagged into the road.)

  Two rules are about topology, which no error bound protects:
  - Closing a hole in a flat surface costs zero geometric error, so open
    edges near the driveline never move (a tunnel window in STK Enterprise
    had been filled in). Separately placed collidable meshes keep all their
    open edges.
  - meshoptimizer's Prune ignores vertex locks: any piece it removes that
    touches the near zone is put back (a bridge on Minigolf had vanished).

  The finer levels of artist LOD groups are replaced by the coarsest one.
  Decoration may be pruned or clustered; collision meshes never are.
- **Kart animation:** keyframes that interpolation reproduces within 3 cm and
  4° are dropped, using the same lerp/slerp as `GE::Armature`. The frame
  markers named in `kart.xml` are always kept.
- **Textures:** resized to ≤64×64 (power-of-two edges, names unchanged).
- **Audio:** converted to libdragon `.wav64` with the RSP's ULC codec. On the
  same track ULC measured 132 KB, against 232 KB for Opus and 780 KB for
  VADPCM.

Measured totals ([budget_report.json](budget_report.json)):

| Content | Stock | Downscaled |
|---|---|---|
| Track meshes, unique triangles per track | 2.5k – 314k | 2.5k – 128k (most 10–60k) |
| Kart meshes | — | 502 – 3,447 triangles (target 500) |
| Music (39 tracks, 61 min) | 70.1 MB | 14.1 MB |
| SFX (68 files) | 5.6 MB | 0.7 MB |
| Collision files, all 44 tracks | — | 28.0 MB |

**Faithfulness** ([tools/validate_downscale.py](tools/validate_downscale.py),
[validate_report.json](validate_report.json)). The check runs headless
4-kart AI races on original and downscaled data with a desktop build:
finishing, rescues (karts reset after falling off or through the track) and
mean finish time, 2 runs per side; a track failing a statistical check is
re-run to 6 runs per side before it counts. The latest run uses the desktop
low-memory build with collision served from the generated files:

- **21 of 21 race tracks pass**, with lap times within ±3.6% of the original
  and no extra rescues.
- **Karts:** none is quite at the ≈500-triangle target. Tux (502) and Gnu
  (515) are close; the rest stall at 526–3,447 triangles because their
  bodies are many disconnected pieces.

## Track collision from ROM

Track collision does not have to fit in RAM. Implemented in
[src/physics/collision_file.hpp](../src/physics/collision_file.hpp):

- **Export (host build).** For each track, the hosted MIPS build writes
  exactly the collision it built in RAM into `n64_collision.bin`:
  - Bullet's quantized tree, with every subtree of up to 8 leaves stored as
    one "bucket" leaf;
  - the triangles, in leaf order, indexing shared vertex and normal tables;
  - a material index per triangle;
  - a hash of the triangle stream the mesh was built from.

  Before keeping a file, the exporter reopens it and checks it against the
  RAM build: every triangle, normal and material, plus Bullet's answers to
  20,000 raycasts and 5,000 box queries over the whole track. All 44 stock
  tracks pass with 0 mismatches.
- **Run time.** The track is converted as usual, but only hashed; the hash
  must match or loading stops with a clear error. Bullet then walks the
  tree and reads triangles through a 128 KB set-associative cache of
  128-byte pages, each miss one DMA from ROM. Buckets are expanded into the
  triangles' own leaves, rebuilt with the same function the tree build uses
  and tested exactly as the full tree would: same triangles, same order.
- **Cost.** A 4-kart Lighthouse race reads 0.2 MB from ROM in all. On the
  desktop, the headless race takes about 20% longer than with collision in
  RAM. Format size: 30 bytes per triangle (the first format took 84, 79 MB
  for all tracks, more than the whole cartridge).

Track scripts are handled the same way: the exporter saves the compiled
AngelScript module (`n64_script.asb`, stamped with a hash of the script
files), and the N64 build loads it. It carries no script compiler or
preprocessor, which halves AngelScript's code size.

## Memory

On the N64, the LTO build's code and static data take 5.9 MB of the 8 MB,
leaving 2,344 KB of heap. Platform setup now costs 65 KB of it: the debug
console's two 32-bit framebuffers (602 KB) are off unless built with
`STK_N64_CONSOLE`.

Peak heap of a 4-kart Lighthouse race, measured with the same code and data
on 32-bit MIPS under QEMU (the N64's pointer size):

| | Peak heap |
|---|---|
| Stock assets | 24.4 MB |
| Downscaled assets | 13.1 MB |
| + `BULLET_SMALL_MEMORY` | 6.6 MB |
| + current asset set, compact collision mesh in RAM | 4.0 MB |
| + collision from ROM, 2 texture layers per material | **3.0 MB** |

What the 3.0 MB peak holds:

| Subsystem | Heap | Note |
|---|---|---|
| Mesh data | 1,139 KB | the main track mesh (639 KB) is held while the track loads |
| STK materials | 326 KB | every material definition |
| XML trees | 263 KB | |
| Bullet broadphase | 216 KB | fixed for 2,048 objects (Black Forest has 1,294) |
| File lists | 208 KB | Irrlicht's directory listings |
| Kart properties | 181 KB | all 18 karts |
| Collision | 147 KB | the ROM cache |
| Scripting | 140 KB | |
| Scene nodes | 135 KB | |

Those were the QEMU measurements. On the N64 itself, with the track's
broadphase sized to its object count (recorded in the collision file), item
models kept without vertex data, and bytecode-only AngelScript, the race now
peaks at **2,661 KB of 2,895 KB** (while the main track mesh is converted;
the romfs index adds 38 KB) and holds about 2.0 MB while racing.

The margin is thin and fragmentation matters more than the total: with
117 KB more code (an experiment with `-O2` on STK's per-frame code), track
objects failed to load with `std::bad_alloc` although 492 KB were free in
total. Every test run is checked for `bad_alloc` as well as for speed.

## Performance

Measured in ares on a seeded 4-kart Lighthouse race (`n64/runs/`), headless
(no rendering, sound or input):

| | |
|---|---|
| Boot to race start | **22 s** (was 140 s) |
| Race | **19.4 frames/s**, 51 ms per frame (was 11) |
| Needed for real time | 60 frames/s: a frame is 1/60 s of game time, 2 physics steps at 120 Hz |

So the race logic alone still runs about **3x slower than real time**.

Race times match the desktop build to within a few seconds; karts take
slightly different lines because the N64's math library rounds differently,
which also makes the track's collision positions differ in the last bits
from the exported file. The N64 build uses the file's
(`STK_COLLISION_FILE_AUTHORITATIVE`); triangle count and materials must
still match exactly.

**Where the time goes** (on-device sampling profiler, `STK_N64_PROFILE`,
report by [tools/n64_profile.py](tools/n64_profile.py); exact self time):

| | Share |
|---|---|
| Track collision: tree walks, ROM cache, triangle tests | 45% |
| Rest of Bullet: broadphase, contacts, solver, vector math | 21% |
| C/C++ runtime: malloc, memcpy, dynamic_cast | 11% |
| STK kart and item logic | 6% |
| AI and drive graph | 5% |
| Kart vehicle physics (btKart) | 4% |

The ray walk over the track's collision tree is 26% on its own: four wheel
rays per kart per physics step, plus the AI's terrain rays. Its samples
cluster at the first instructions of code blocks, the signature of
instruction-cache misses. FPU traps are not a cost: in a whole race the
VR4300 trapped on one operation (NaN/denormal operand, completed in
software by `fpu_emu.c`); loading accounts for the other 1,372.

**What got it from 11 to 19 frames/s, and startup from 140 s to 22 s:**

- *Collision buckets kept decoded* (256 buckets, 33 KB, 98% hits): every
  bucket a query touched was being decoded and quantized again.
- *Tree nodes read in place* from the page cache, a page of nodes at a time
  (`btExternalBvhNodes::nodeSpan`), with a cursor per read stream; no
  library `memcpy` per 16-byte node.
- *Compiler levels*, each measured on the same seeded race: Bullet and
  `src/physics` at `-O2` (29% faster than `-Oz`). Higher is worse here: STK's
  own per-frame code at `-Os` made the race 8% slower than `-Oz` (the
  VR4300's instruction cache is 16 KB), and `-O3` adds 92 KB of code, after
  which track loading runs out of memory. libdragon's
  `-ftrivial-auto-var-init=pattern` is off for STK (it filled every local
  on every call).
- *Startup*: `stat()` on `rom:/` answered from an in-RAM index of the
  cartridge filesystem (see the platform layer); DragonFS read directory
  entries from ROM one 256-byte DMA at a time, 90% of startup.

**Testing notes.** STK seeds its random numbers with `time(0)`, the seconds
since boot here, so the test race passes `--seed=1`. Even so, a race only
repeats exactly while the sequence of heap allocations does: STK orders
some things by address (the desktop build differs run to run under ASLR).
Changes to code that allocates are compared in milliseconds per frame.

The profiler's own pitfalls, now handled: sampling through ares's GDB
server is not proportional to N64 time; and the sampler must not use
stdio or `io_write()` from its interrupt (both wait on kernel objects,
which corrupted the scheduler and hung the race in a DMA wait).

## Platform layer (`n64/compat`)

STK's code is used as is. What the N64 lacks is provided underneath it.

- **Threads:** `std::thread`, `std::mutex`, `std::condition_variable` and
  `std::call_once` run on libdragon's kernel.
  - libstdc++ reaches its thread model through `bits/gthr-default.h`, so a
    replacement for that header plus GCC's own `thread.cc`,
    `condition_variable.cc` and `mutex.cc` turn threading on, with no
    toolchain change.
  - Joins use a mutex and condition variable, because `kthread_join` at this
    libdragon revision parks the joining thread on no wait list.
  - Kernel types are opaque in the header, so libdragon's macros (it
    redefines `srand`) do not leak into STK.
- **Time:** `std::chrono` and `gettimeofday` use the CPU counter. libdragon's
  `gettimeofday` needs a cartridge RTC, and without one it leaves its output
  uninitialised.
- **Networking, off but switchable:**
  - the POSIX socket API, netdb and poll are present and report "network
    down";
  - curl and mbedtls are stubs, with real SHA-256 and base64;
  - STK's network code compiles unchanged and takes its normal offline path.

  Real networking later means replacing `net_stub.c` and the stubs.
- **Files:**
  - `rom:/` (DragonFS) holds the data, read-only;
  - `ram:/` (`ramfs.c`) is writable and holds config, profiles and the log;
  - `dirent`, `realpath`, `access`, `fcntl` and `uname` are filled in.
- **ROM file lookups** ([romfs_index.c](compat/src/romfs_index.c)):
  DragonFS finds a path by reading its directory entries from ROM one at
  a time, a DMA each. A table of 64-bit path hashes built at boot (2.7 s,
  38 KB for 3,090 entries) answers `stat()` for `rom:/` instead, replaying
  DragonFS's own path walk line for line, quirks included.
  `STK_N64_VERIFY_ROMFS_INDEX` checks every answer against the real
  `stat()`: 0 mismatches over a whole race.
- **Log:** stdout (STK's log) goes to the debug channel (ISViewer/USB), or
  to the on-screen console in `STK_N64_CONSOLE` builds. With
  `STK_N64_LOG_HEAP` each line starts with the heap in use and the time
  since boot.
- **Profiling:** `STK_N64_PROFILE` (timer-driven sampler, report by
  [tools/n64_profile.py](tools/n64_profile.py)) and `STK_N64_LOG_DMA` (ROM
  reads counted and timed by region); both off by default.
- **C++ exceptions with LTO:** libgcc hands landing pads a zero-extended
  exception pointer. On o64, with libdragon's 64-bit addressing, that is an
  invalid address. Four runtime entry points are wrapped to sign-extend it.
- **Diagnostics:** an uncaught exception, `abort` or `exit` halts with its
  message on the debug channel, instead of making ares reboot the ROM.

STK source changes, all inert on PC unless a low-memory flag is set
(desktop races unchanged):

- `XMLNode::get()` gains `int`/`unsigned` overloads for targets where
  `int32_t` is `long`.
- `splitPath()` does not split on `:` under `N64`.
- The `.spm` loader is big-endian safe.
- `STK_LOW_MEMORY`:
  - an indexed, de-duplicated collision mesh with packed normals and a
    quantized tree;
  - collision and track scripts from files (`collision_file.*`,
    `ScriptEngine::compileLoadedScripts`);
  - 2 texture layers per Irrlicht material instead of 8.
- Bullet `BULLET_SMALL_MEMORY`:
  - smaller preallocated pools;
  - tree leaves reserved exactly;
  - hooks for trees and triangles that live outside memory.

## Client (the full game)

The client is STK's normal build with its fixed-function (legacy OpenGL)
renderer, on libdragon's OpenGL through an SDL2 back-end
([compat/src/sdl_n64.c](compat/src/sdl_n64.c)): display 320x240 16-bit
double-buffered, the controllers as SDL joysticks with a game-controller
mapping (STK's console layout: A accelerate, B brake, Z fire, R drift; the
stick steers and moves through menus), no audio device yet.

What it takes to fit 6.6 MB of code and the game's data in 8 MB:

- **Code paging** ([compat/src/pager.c](compat/src/pager.c)): everything but
  libdragon, newlib, libgcc and the platform layer is linked at a TLB-mapped
  address and stays on the cartridge, with its constants and C++ exception
  tables; 8 KB blocks are read into a 1 MB pool when first touched, and
  TLB refills of loaded blocks are served by the refill vector. Small
  blocks matter: a menu frame touches hundreds of scattered functions, and
  32 KB blocks made its working set larger than the pool (1 frame/s); with
  8 KB the menus run at 8-10 frames/s with no page faults.
- **Textures** are libdragon sprites in the smallest exact format: CI4 for
  16 colours (64x64 in 2 KB), CI8 / RGBA16 at 2048 texels otherwise, IA8
  for font pages and soft grey masks (shadows). The cartridge's images are
  colour-reduced when the ROM is built ([tools/n64_gui_images.py](tools/n64_gui_images.py)),
  so the console only counts colours.
- Text is sized as on a 640x480 screen (the smallest STK lays its menus
  out for); at 320x240 it would be 5 pixels high.
- Lazy or dropped data: the Thai dictionary and country names load on
  demand; fonts for non-Latin scripts and CJK/emoji are left off the
  cartridge while translations are off; kart animation keyframes that
  interpolation reproduces are dropped as they are loaded; vertex arrays
  are allocated at their final size; the height-map collision tree is only
  built when weather particles use it.
- First start uses STK's lowest graphics preset; render-to-texture (the
  minimap) works by copying from the frame.

Races (2 karts, the profiling build in ares, 1 MB code pool):

| Track | State |
|---|---|
| Snow Peak, Shifting Sands, Snow Mountain, Mini Golf, Hacienda, Fort Magma | race runs (4-7 frames per 10 s) |
| Lighthouse, Zen Garden | out of memory while loading scenery: library objects (pine trees: 2,362 triangles at their finest level, cap 337) and LOD groups keep far more geometry than the asset budget allows |

Known gaps: no audio; the focus highlight of icon menus is not drawn; a race
runs at under 1 frame/s (page faults: ~450/s; CPU); some key presses are
lost at low frame rates.

## Next steps

1. Client memory: make the asset pipeline meet its own budgets for library
   objects and LOD groups (the finest levels of a group replaced by the
   coarsest, as planned), so every track fits; then measure how small the
   code pool can be in a race.
2. Client speed: race page faults (lay hot code out together, from the
   profiler's samples), then the CPU work below.
3. Audio back-end (libdragon mixer behind SDL audio), the menu focus
   highlight.
4. Speed, 3x to go for the headless race:
   - the collision ray walk (26%) and the rest of the collision path;
   - per-frame allocations (InterpolationArray copies in the kart and AI
     code) and `dynamic_cast`;
   - if that is not enough, a decision on fidelity: physics at 60 Hz
     instead of 120 Hz would halve the physics cost but changes how karts
     handle.
5. After the race, STK's shutdown stalls after its last "Destroyed
   singleton" and `stk_main` never returns (harmless for a game that is
   switched off, but it hides the end-of-run report).
6. Memory headroom (~230 KB at the loading peak), needed for the renderer:
   - trim libstdc++'s locale;
   - avoid holding the whole main track mesh while it is converted.
7. Renderer: the client draws through libdragon's OpenGL; if that is too
   slow, a back-end for Irrlicht's scene on Tiny3D, with render geometry
   streamed from ROM like collision.
8. Karts that stall above the triangle budget: a no-pruning fallback, or
   joint culling.
9. Cartridge budget: collision (28 MB) plus meshes, audio and textures must
   fit in 64 MB.
