# SuperTuxKart on the N64: port status

Goal: all stock content, downscaled to N64 budgets; networking off but
switchable back on; tested in ares using the build scripts from
[atyr647/Pak](https://github.com/atyr647/Pak).

## Where it stands

| Area | State |
|---|---|
| Toolchain, ares | Built with Pak's `tools/build_n64_toolchain.sh` and `tools/build_ares.sh` (GCC 16.2.0, ares with the N64 core) |
| libdragon | `preview` branch, pinned ([build_libdragon.sh](build_libdragon.sh)). Pak pins `trunk`, which has no kernel threads or OpenGL. |
| STK on libdragon | Compiles and links (headless `SERVER_ONLY` build). In ares it runs STK's real `main()` into race loading: config, offline networking, Irrlicht, karts, the track's scene and its collision, served from ROM. |
| Blocker | **RAM.** The headless race still runs out of heap while setting up physics (see [Memory](#memory)). |
| Assets | Every stock track, kart, texture and sound is converted ([tools/stk_n64_assets.py](tools/stk_n64_assets.py)). **All 21 race tracks validated as faithful.** |
| Collision | Served from cartridge ROM through a 128 KB cache, bit-for-bit what STK would build in RAM ([details](#track-collision-from-rom)). |

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

So the headless race needs about 0.7 MB more than the N64 has. In ares the
ROM gets through loading the track and opening its collision, then runs out
in `Physics::init`. The next savings in hand:

- AngelScript without its compiler (done in this tree, not yet measured on
  the N64): about 300 KB of code;
- libstdc++'s locale facets (`time_get`, `money_get`, ... for both `char`
  and `wchar_t`, in both ABIs), which STK never uses: about 250 KB of code;
- not keeping the whole main track mesh while it is converted, which the
  ROM collision makes possible (the client renderer needs the same:
  streaming render geometry from ROM).

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
- **Log:** stdout (STK's log) goes to the debug channel (ISViewer/USB), or
  to the on-screen console in `STK_N64_CONSOLE` builds.
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

## Next steps

1. Memory, to finish a headless race in ares:
   - measure the bytecode-only AngelScript build on the N64;
   - trim libstdc++'s locale;
   - avoid holding the whole main track mesh while it is converted.
2. Renderer: an N64 back-end for Irrlicht's scene, using libdragon GL or
   Tiny3D, with render geometry streamed from ROM like collision. Then input
   and audio back-ends, and the client (non-server) build.
3. Karts that stall above the triangle budget: a no-pruning fallback, or
   joint culling.
4. Cartridge budget: collision (28 MB) plus meshes, audio and textures must
   fit in 64 MB.
