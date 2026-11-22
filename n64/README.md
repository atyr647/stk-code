# SuperTuxKart on the Nintendo 64: scoping report

> **Port in progress:** see [PORT_STATUS.md](PORT_STATUS.md) for the libdragon
> build, the asset pipeline, and what currently runs in ares.

**Goal:** run SuperTuxKart (STK) on N64 hardware as faithfully as possible,
with as few source changes as possible.

**Bottom line:**

- **The code side is in better shape than expected.** STK's C++ code, and
  every bundled or third-party library, compiles and links for the N64's CPU
  (NEC VR4300, MIPS III, big-endian). It runs correctly under emulation once
  one big-endian bug is fixed, which this branch does. Unit tests pass, and
  full AI races finish with results consistent with a native x86 build.
- **The hardware side rules out a faithful port on a stock N64.** Measured on
  this codebase:
  - The game *logic alone* (no graphics or sound) needs about 1.2× the CPU's
    theoretical peak.
  - Its working memory is about 6–12× the console's entire RAM.
  - One kart model has more triangles than an N64 frame can usually
    draw; a race scene draws about 119k triangles per frame.
  - The art assets are about 11× the largest retail cartridge.
- **A recognisable "STK-lite" is possible**, mostly by reusing STK's
  gameplay code on top of N64-native assets and a libdragon rendering
  back-end. It is a large project, though, not a recompile.

Everything below was measured on this branch, except where marked
*(estimate)* or *(published spec)*.

---

## 1. What was done

| Step | Result |
|---|---|
| Cross-compiler | Ubuntu `mips-linux-gnu-g++` 12.4 with `-march=vr4300 -mtune=vr4300 -mfix4300 -EB`. Objects are MIPS III, o32, big-endian. |
| Dependencies | zlib, libpng, libjpeg-turbo, freetype, harfbuzz, ogg, vorbis, SDL2, mbedtls, curl and libsamplerate, all built as static libraries with [`build_mips_deps.sh`](build_mips_deps.sh). |
| Server-only build (`-DSERVER_ONLY=ON`) | **0 errors, 0 source changes.** 12.9 MB of code (`-O2`). |
| Full client build (Irrlicht, Vulkan/GL back-ends, GUI, audio) | **0 compile errors.** It links once the dependency build passes `-lvorbis -logg` at the end of the link line (a build-command flag, not a source change). 21.5 MB of code (`-O2`). |
| Running under `qemu-mips` (big-endian) | First run aborted: `SPMeshLoader: Not little endian machine.` **Fixed in this branch** (see §2). |
| `--unit-testing` under qemu | **All tests pass**: MiniGLM, NetworkString, SocketAddress, ArenaGraph, Kart characteristics, PowerupManager, RewindQueue and the rest. |
| Headless 4-kart AI race (Lighthouse track) under qemu | **Completes.** Physics, AI, items and lap logic all work, in both the server and client binaries. Results fall in the same range as native x86. In one MIPS run, the leading kart's time, average speed and top speed matched native exactly (53.09 s / 16.57 / 20.00). STK races are not fully deterministic between runs even natively, because of threads. |
| Native x86 control build | Used for memory measurements and to check whether oddities were MIPS-specific. |

An oddity that turned out *not* to be a port bug: the profile-mode
"explosion time" statistic sometimes prints huge or negative numbers. It
does the same on native x86, so it is a pre-existing STK bug in that
statistic.

### Caveat on the toolchain

The Ubuntu cross-compiler targets a *hosted Linux* MIPS system, so the
binaries can run under qemu. Its C library is built for MIPS32r2, which is
not a superset of MIPS III, so the test link uses `-Wl,--no-warn-mismatch`.
The STK objects themselves are pure VR4300 code.

A real N64 build needs **libdragon's bare-metal `mips64-elf` toolchain**
(newlib, `-mabi=o64`). I cloned libdragon (`preview` branch) to check what
it provides. Running its `tools/build-toolchain.sh` was blocked by this
session's permission policy, so you need to build it yourself (about 30 min):

```sh
git clone -b preview https://github.com/DragonMinded/libdragon
cd libdragon && export N64_INST=$HOME/n64 && ./tools/build-toolchain.sh
```

## 2. Source changes made (one file pair)

`.spm` (STK's mesh format) is little-endian on disk, and the loader read
raw bytes straight into C++ variables. It also read 1- and 2-byte fields
into 4-byte variables, which only works on little-endian CPUs. This branch
adds `GE::readLE()` / `GE::readLEAs()` in `lib/graphics_engine/include/ge_animation.hpp`
and uses them for every multi-byte field in `src/graphics/sp_mesh_loader.cpp`
and the armature loader.

On little-endian machines behaviour is identical to before: `readLE`
compiles to a plain `read`. Native unit tests and races are unchanged.

STK already has explicit byte-order handling for Bullet BVH caches, Ogg
decoding and translations. Network packet serialisation was verified by the
`NetworkString` and `SocketAddress` unit tests under big-endian emulation. The one
remaining raw-binary loader is the legacy `.b3d` one, which only a single
file in the current assets still uses.

## 3. The hardware gap (measured)

N64 reference *(published spec)*:
- **CPU:** 93.75 MHz VR4300; scalar, at most 1 instruction per clock, in
  practice well below that because of the 16 KB instruction / 8 KB data
  caches and slow RDRAM.
- **RAM:** 4 MB, or 8 MB with the Expansion Pak, shared by code, data, frame
  buffers, Z-buffer, textures and audio.
- **Graphics:** the RCP at 62.5 MHz. Its RSP vector unit runs geometry and
  audio microcode; its RDP rasterises with a **4 KB texture cache**, which
  fits a 64×64 16-bit texture.
- **Cartridge:** up to 64 MB.

### 3.1 CPU: game logic alone exceeds the CPU

Executed MIPS instructions were counted with a QEMU plugin. Two runs, of 20
and 40 s of game time, were subtracted so that loading cancels out. In
profile mode the game advances a fixed 1/60 s per frame, so this is exact.

| Configuration (4 AI karts, Lighthouse, no graphics, no audio) | Instructions per game-second |
|---|---|
| Stock (`physics fps="120"`) | **108.9 M/s**, vs. 93.75 M/s theoretical peak |
| `physics fps="60"` (a data-only change in `stk_config.xml`) | **53.4 M/s** |

Where the time goes, at 120 Hz:

| Component | Share |
|---|---|
| Bullet collision | 64.8 % (37 % in one function: the ray-vs-track-mesh tree walk used for wheel raycasts) |
| libc / libm / libstdc++ | 15.7 % |
| `src/karts` (kart logic, AI) | 6.9 % |
| `src/physics` | 4.2 % |
| `src/tracks` | 3.4 % |
| Bullet dynamics | 1.9 % |
| `src/items` | 1.9 % |

Instruction count *understates* real time on a VR4300. Floating-point
multiplies, divides and square roots take several to dozens of cycles, and
cache misses stall the pipeline. So even at 60 Hz physics, game logic would
use most or all of the CPU *(estimate)*, leaving nothing for animation,
scene management, sound mixing or feeding the GPU.

The biggest single lever is **collision-mesh complexity**. Wheel raycasts
walk a tree built over the track's full collision mesh, which is tens of
thousands of triangles on Lighthouse. An N64-sized track (a few thousand
triangles) would cut that cost substantially, with no code change.

### 3.2 Memory: about 6–12× too big before any textures

| Measurement | Value |
|---|---|
| Code, client, `-O2` | 21.5 MB (STK `src/` 11.0 MB, Irrlicht 3.4, GE/Vulkan 1.4, angelscript 1.2, harfbuzz 1.2, SDL2 1.4, ...) |
| Code, client, `-Os` + unused-section removal, stripped | **15.2 MB** (+1.3 MB data/bss) |
| Code, "core" subset at `-Os`: game code minus `network/`, `online/`, `addons/`, `states_screens/`, plus Irrlicht, Bullet, AngelScript | **≈8.6 MB** (before unused-section removal; excludes libc/libstdc++) |
| Native peak memory, server build, 4 karts on Lighthouse (**loads no textures**) | **47 MB** (17.5 MB just to start up) |
| Lighthouse track textures, track folder only | 38 textures, 11.3 Mpixel: 43 MB as RGBA8, 22 MB as 16-bit |
| Tux kart textures | 3.8 Mpixel: 14 MB as RGBA8 |

The N64 has 4 or 8 MB *in total*. Even the size-optimised client code
alone does not fit, before any data or assets. A stripped-down build would
have to load code from the cartridge on demand; libdragon supports this
through its `dlopen` loader (`dlfcn.h`).

### 3.3 Rendering: STK scenes are an order of magnitude too heavy

Measured with a native x86 client using STK's **legacy fixed-function
renderer** (`force_legacy_device`), the same code path an N64 port would
use. Settings: Mesa llvmpipe, **320×240**, Lighthouse, 4 karts, a full
profiled lap. Irrlicht's driver counted **≈119,000 triangles drawn per
frame** on average.

That is more than the track folder alone (below), because tracks also place
shared `library/` objects such as trees and props. A typical N64 frame is a
few thousand triangles, so this is **roughly 30–40× over budget**.


- **Tux kart model:** 6,238 triangles, with 1024×1024 textures.
- **Lighthouse track:** 46,236 triangles (all `.spm` meshes in the track
  folder).
- **N64 practical budget** *(published / commonly cited)*: a few thousand
  textured triangles per frame at 20–30 fps, with textures of 64×64 or less
  (4 KB texture cache).

**Renderer compatibility is the good news.** STK still ships a
*fixed-function OpenGL* path: Irrlicht's `COpenGLDriver`, used when the GPU
lacks GLSL 3.x support or `--force-legacy-device` is set. libdragon provides
an **OpenGL 1.1 implementation (with `ARB_vertex_buffer_object`) running on
the RSP/RDP**. That is the closest match available between STK's existing
rendering code and N64 hardware. Gaps to close:
- Irrlicht's GL extension probing;
- render-to-texture (the minimap);
- 32-bit colour assumptions;
- and, decisively, asset sizes.

### 3.4 Storage

The official asset repository totals **≈710 MB**, measured from an SVN
listing that excludes work-in-progress folders:

| Folder | Size |
|---|---|
| `textures/` | 283 MB |
| `tracks/` | 277 MB |
| `music/` | 67 MB |
| `karts/` | 38 MB |
| `library/` | 34 MB |
| `sfx/` | 5 MB |
| `models/` | 3 MB |

The largest N64 cartridge is 64 MB, so this is about 11× too big, even
before converting to N64 texture formats.

## 4. Platform-layer work for libdragon (what "minimal changes" means)

| STK dependency | Uses | N64 / libdragon answer | Change size |
|---|---|---|---|
| Irrlicht device (SDL2) | `CIrrDeviceSDL` | New `CIrrDeviceN64`: display init, joypad → Irrlicht events, timer. The `CIrrDeviceServer` stub is a good template. | New file, ~500 lines *(estimate)* |
| Video driver | `COpenGLDriver` (legacy) | Build it against libdragon's `GL/gl.h`, and strip or stub the extensions libdragon lacks. | Moderate, mostly `#ifdef`s |
| Vulkan "GE" back-end, shaderc | `lib/graphics_engine` | Not applicable. Compile it out (as `SERVER_ONLY` already does). | Build-flag work |
| Audio | OpenAL / MojoAL + Ogg Vorbis | libdragon `mixer` + `wav64`/`xm64`. Vorbis decoding in software would cost too much CPU. | New `SFXManager` back-end |
| Threads | `std::thread` in 23 files (audio, network, addons, online requests) | libdragon has kernel threads (`kthread`, `kmutex`). Most users are networking or online, which get compiled out anyway. | Small |
| Networking, online, addons | ENet, sockets, curl, mbedtls | No network hardware. Compile out `src/network`, `src/online` and `src/addons`, or stub them. | Medium: the lobby/rewind code is wired into race logic |
| Filesystem | `FileManager`, Irrlicht FS | libdragon DragonFS (`rom:/`). Read-only, so saves go to EEPROM or SRAM. | Small–medium |
| Scripting | AngelScript (track scripts) | AngelScript's native calling convention supports MIPS o32. libdragon uses o64, so expect `as_callfunc_mips.cpp` work, or use AngelScript's portable `AS_MAX_PORTABILITY` mode. | Small–medium |
| C++ features | exceptions in 58 files, `dynamic_cast` in 107 | libdragon supports C++ with exceptions and RTTI. | None |
| Endianness | `.spm` loader | **Done (this branch).** Vertex-colour packing for GL upload still needs checking on real hardware. | Done |

## 5. Options

1. **Faithful port on stock hardware: not feasible.** Every resource
   measured is over budget by a large factor (CPU 1.2× *before* rendering,
   RAM 6–12×, triangles per frame 30–40×, storage about 11×). No amount of build
   tweaking closes that.
2. **STK-lite: STK gameplay code plus N64-native content (recommended if
   you want to pursue this).**
   - Reuse kart physics, AI, items, race modes and the track and checkpoint
     logic largely unchanged; that code is portable, as shown above.
   - Run physics at 60 Hz (a data change).
   - Rebuild a few tracks and karts at N64 scale: about 2–5 k triangles per
     track section, 300–600 per kart, 32×32 / 64×64 textures, and simple
     collision meshes. STK's own track format and loaders can be kept.
   - Write `CIrrDeviceN64`, point `COpenGLDriver` at libdragon's GL, and use
     a libdragon audio back-end.
   - Compile out networking, online, addons, the Vulkan engine and most GUI
     screens, which also brings code size into range.
   - Target the 8 MB Expansion Pak.
   - Even then, frame rate is the open risk. Prototype the physics loop on
     real hardware or an accurate emulator (ares) early.
3. **Pragmatic alternatives that are much more faithful:** the
   Switch/Android ports already in this repo, or a low-end ARM handheld
   running Linux, run STK unmodified.

## 6. Reproducing

```sh
sudo apt install g++-mips-linux-gnu qemu-user cmake
# clone the dependency sources (tags listed in build_mips_deps.sh) into ~/mipsdeps/src
n64/build_mips_deps.sh ~/mipsdeps/src ~/mipsdeps/prefix
export MIPS_DEPS_PREFIX=~/mipsdeps/prefix
cmake -S . -B build-mips -DCMAKE_TOOLCHAIN_FILE=n64/toolchain-mips-vr4300.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$MIPS_DEPS_PREFIX \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lvorbis -logg -lm" \
  -DCHECK_ASSETS=OFF -DUSE_SQLITE3=OFF -DUSE_SYSTEM_ENET=OFF -DUSE_SYSTEM_MCPP=OFF \
  -DUSE_WIIUSE=OFF -DUSE_DNS_C=ON -DUSE_CRYPTO_OPENSSL=OFF -DNO_SHADERC=ON -DUSE_MOJOAL=ON \
  -DSERVER_ONLY=ON        # drop this line for the full client
cmake --build build-mips -j$(nproc)
# needs a data dir that also contains stk-assets (karts/ tracks/ textures/ ...)
SUPERTUXKART_DATADIR=/path/to/root QEMU_LD_PREFIX=/usr/mips-linux-gnu \
  qemu-mips build-mips/bin/supertuxkart --unit-testing
SUPERTUXKART_DATADIR=/path/to/root QEMU_LD_PREFIX=/usr/mips-linux-gnu \
  qemu-mips build-mips/bin/supertuxkart --no-graphics --track=lighthouse \
  --numkarts=4 --kart=tux --profile-laps=1
```
