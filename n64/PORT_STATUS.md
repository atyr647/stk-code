# SuperTuxKart on the N64: port status

Goal: all stock content, downscaled to N64 budgets; networking off but
switchable back on; tested in ares using the build scripts from
[atyr647/Pak](https://github.com/atyr647/Pak).

## Where it stands

| Area | State |
|---|---|
| Toolchain, ares | Built with Pak's `tools/build_n64_toolchain.sh` and `tools/build_ares.sh` (GCC 16.2.0, ares with the N64 core) |
| libdragon | `preview` branch, pinned ([build_libdragon.sh](build_libdragon.sh)). Pak pins `trunk`, which has no kernel threads or OpenGL. |
| STK on libdragon | Compiles and links. In ares it runs STK's real `main()`: file system, config, offline networking, Irrlicht, GUI skin, SFX/music tables and player profiles, up to kart loading. |
| Blocker | **RAM.** Code and static data take ≈6.0 MB of 8 MB, and a downscaled race needs ≈6.6 MB of heap (see [Memory](#memory)). |
| Assets | Every stock track, kart, texture and sound is converted ([tools/stk_n64_assets.py](tools/stk_n64_assets.py)). 15 of 21 race tracks validated as faithful; 6 still have collision defects. |

## Building and running

```sh
# once: toolchain and ares from Pak, then libdragon preview and zlib
JOBS=4 ~/Pak/tools/build_n64_toolchain.sh /opt/pak-n64
~/Pak/tools/build_ares.sh /opt/pak-ares
N64_INST=/opt/pak-n64 n64/build_libdragon.sh
n64/build_n64_deps.sh <zlib source> /opt/n64deps

# downscale the stock assets (needs n64/tools/meshsimplify built against
# meshoptimizer v0.22, plus oggdec)
python3 n64/tools/stk_n64_assets.py --assets stk-assets --out n64data

# build: LTO saves ~1 MB of RAM
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

[tests/threads](tests/threads) is a regression ROM for the threading layer;
it passes in ares.

## Asset pipeline (all stock content)

The output is an ordinary STK data tree: same layout, XML and file names, so
the unmodified desktop game loads it. That makes the PC game the reference for
"did downscaling break anything".

- **Meshes:** decimated with meshoptimizer, which only removes triangles, so
  UVs, normals, colours and skinning weights stay exact. Each track mesh has
  three error zones:
  - the driving surface (driveline quads): at most 3 cm;
  - walls and barriers within 20 m, and every mesh karts collide with,
    including invisible `physics-only` walls: at most 25 cm;
  - scenery: unlimited.

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
| Track meshes, unique triangles per track | 2.5k – 314k | 2.5k – 99k (most 6–30k) |
| Triangles drawn per frame, Lighthouse (desktop legacy renderer) | 118.6k | 16.8k |
| Kart animation, all karts | 5.0 MB | 2.9 MB |
| Music (39 tracks, 61 min) | 70.1 MB | 14.1 MB |
| SFX (68 files) | 5.6 MB | 0.7 MB |
| Meshes, all tracks and karts | — | 27.7 MB |
| Textures as N64 sprites (CI4, all 2,115) | — | ≈8.6 MB |

**Cartridge estimate:** 27.7 MB meshes + 14.8 MB audio + 8.6 MB textures +
7.5 MB XML + ≈3 MB code = ≈61 MB uncompressed, against 64 MB. Compressing
meshes and XML in DragonFS would leave real headroom.

**Faithfulness** ([tools/validate_downscale.py](tools/validate_downscale.py),
[validate_report.json](validate_report.json)). The check runs headless
4-kart AI races on original and downscaled data with a desktop build:

- **15 of 21 race tracks pass**: they finish, with no extra rescues and lap
  times within 10% (most within ±3%). These include Abyss, Lighthouse,
  Ravenbridge, Mines, Volcano Island and Gran Paradiso.
- **6 do not finish on downscaled data:** Black Forest, Candela City, Cocoa
  Temple, Cornfield Crossing, Minigolf and STK Enterprise. On Minigolf a kart
  ends up stuck above a "reset" surface, i.e. a gap opened in a mesh karts
  touch. Next step: bisect per mesh (as was done for Abyss) and tighten
  whichever rule that mesh fell under.
- **Karts:** 7 of 18 reach the ≈500-triangle target. The rest stall at
  536–3,442 triangles because their bodies are many disconnected pieces.

## Memory

Measured with the same code and data on 32-bit MIPS under QEMU, which has the
same pointer size as the N64:

| | Peak heap, 4-kart Lighthouse race |
|---|---|
| Stock assets | 24.4 MB |
| Downscaled assets | 13.1 MB |
| Downscaled assets, `BULLET_SMALL_MEMORY` | **6.6 MB** (lap times unchanged) |

On the N64, the LTO build's code and static data are 6.0 MB, leaving ≈2.0 MB
of heap. STK's static initialisation uses 0.66 MB of that before `main`.

Remaining large heap users, by allocation site (cumulative):

| Allocation site | Total | Cause |
|---|---|---|
| Bullet `btAlignedObjectArray`s | 4.7 MB | Track collision mesh and BVH, grown by doubling |
| `SPMeshLoader::decompress` | 1.2 MB | Mesh buffers |
| `std::vector<btVector3>` | 1.0 MB | Collision normals, grown by doubling |
| Irrlicht XML reader | 0.8 MB | Files are widened to 4-byte characters; transient |

Closing the ≈5 MB gap needs targeted changes inside STK:
- size these arrays up front instead of letting them double;
- stream XML instead of widening whole files;
- load cold code (menus, achievements, online) on demand through libdragon's
  DSO loader instead of keeping it resident.

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
- **C++ exceptions with LTO:** libgcc hands landing pads a zero-extended
  exception pointer. On o64, with libdragon's 64-bit addressing, that is an
  invalid address. Four runtime entry points are wrapped to sign-extend it.
- **Diagnostics:** an uncaught exception, `abort` or `exit` halts with its
  message on screen and on the debug channel, instead of making ares reboot
  the ROM.

STK source changes, all inert on PC (desktop unit tests and races
unchanged):

- `XMLNode::get()` gains `int`/`unsigned` overloads for targets where
  `int32_t` is `long`.
- `splitPath()` does not split on `:` under `N64`.
- The `.spm` loader is big-endian safe (earlier commit).
- Bullet's `BULLET_SMALL_MEMORY` shrinks the preallocated pools.

## Next steps

1. Memory:
   - presize Bullet/STK arrays;
   - stream XML;
   - move menus and online code into DSOs.

   Target: a full race in ≈2 MB of heap.
2. Fix collision defects on the 6 failing tracks. Then add a no-pruning
   fallback, or joint culling, for karts that stall above budget.
3. Renderer: an N64 back-end for Irrlicht's scene, using libdragon GL or
   Tiny3D. Then input and audio back-ends, and the client (non-server)
   build.
