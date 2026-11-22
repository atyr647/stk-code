#!/usr/bin/env python3
"""Downscale the stock SuperTuxKart assets to Nintendo 64 budgets.

The output is still an ordinary STK data tree -- same directory layout, same
XML, same .spm and image formats, same file names -- so the unmodified game
loads it, on a PC as well as on the N64. That is deliberate: the desktop build
is the oracle for "did downscaling break the track" (karts falling through
holes, missing materials, unreachable checkpoints) before anything is tried on
the console.

What changes:
  * every .spm mesh is decimated with meshoptimizer (n64/tools/meshsimplify),
    which only ever removes triangles, so surviving vertices keep their exact
    UVs, normals, colours and skinning weights;
  * within an artist LOD group, finer levels are replaced by the coarsest one;
  * textures are resized to N64 texture-cache-sized images (default 64x64,
    shrunk further when a track uses many), keeping their names so
    materials.xml lookups (zippers, terrain sounds, ...) still match;
  * nothing is removed that gameplay depends on.

Budgets (per track, see BUDGETS) cap the *unique* triangles loaded (memory,
physics) and the *instanced* triangles in the scene (drawing). Mesh shared by
several tracks (library/, models/) are decimated once, conservatively.

  python3 n64/tools/stk_n64_assets.py --assets ../stk-assets --out build-n64-data
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spm          # noqa: E402
import stk_scene    # noqa: E402

# ----------------------------------------------------------------------------
BUDGETS = {
    # whole-track unique triangles: what has to sit in RAM and in Bullet's BVH
    "track_unique_tris": 9000,
    # whole-track instanced triangles (coarsest LOD): upper bound on drawing
    "track_scene_tris": 24000,
    # never decimate a mesh below this many triangles (or below its original)
    "min_mesh_tris": 12,
    # driveable surface (driveline quads) may deviate at most this much
    "road_error_m": 0.03,
    # walls/barriers next to the road, and every other mesh karts collide with
    "collision_error_m": 0.25,
    "near_road_m": 20.0,
    # far scenery: largest component (fraction of track extent) prune may drop
    "far_prune_error": 0.02,
    # karts: body + all parts, per kart
    "kart_tris": 500,
    # shared models/ (items, powerups) and library objects: per mesh cap
    "model_tris": 160,
    "library_ratio": 0.12,
    # textures: max edge and texel cap per image (64x64 CI4 = 2 KB)
    "texture_max_edge": 64,
    "texture_texels": 64 * 64,
    "texture_texels_crowded": 32 * 32,   # tracks with many textures
    "crowded_texture_count": 90,
}

MESHSIMPLIFY = os.environ.get("MESHSIMPLIFY", "meshsimplify")
IMAGE_EXT = (".png", ".jpg", ".jpeg")


# ----------------------------------------------------------------------------
# Mesh decimation
# ----------------------------------------------------------------------------
def _run_meshsimplify(pos, att, weights, indices, vc, target_tris, options,
                      lock=None, target_error=1.0):
    ac = len(weights)
    hdr = struct.pack("<IIIIfI", vc, len(indices), ac,
                      max(0, target_tris * 3), target_error, options)
    payload = (hdr + bytes(pos) + bytes(att) +
               struct.pack("<%df" % ac, *weights) +
               (bytes(lock) if lock is not None else bytes(vc)) +
               struct.pack("<%dI" % len(indices), *indices))
    r = subprocess.run([MESHSIMPLIFY], input=payload, capture_output=True,
                       check=True)
    n, _err = struct.unpack_from("<If", r.stdout, 0)
    return list(struct.unpack_from("<%dI" % n, r.stdout, 8))


PRUNE = 1 << 3          # meshopt_SimplifyPrune: may drop tiny isolated parts
SLOPPY = 1 << 31        # meshsimplify: use meshopt_simplifySloppy


def decimate(mesh, target_tris, lock_border=False, road=None,
             allow_sloppy=False, max_error_m=None):
    """Decimate a whole mesh towards target_tris.

    All buffers are simplified together, over one concatenated vertex array.
    Buffers do not share vertices, but meshoptimizer recognises vertices with
    equal positions as seams and only collapses them in step, so material
    boundaries (road/grass, ...) stay watertight instead of cracking apart --
    which matters for Bullet as much as for the picture. A collapse never
    moves a triangle onto another buffer's vertices; that is checked, and a
    mesh that would violate it is left alone.
    """
    total = mesh.tri_count
    if total <= target_tris or total == 0:
        return mesh
    bufs = list(mesh.buffers())
    base, owner = [], []
    pos, att, all_idx = bytearray(), bytearray(), []
    weights = [1.0, 1.0] + ([0.5, 0.5, 0.5] if mesh.read_normal else [])
    off = 0
    for bi, b in enumerate(bufs):
        uv1, _uv2 = spm._uv_flags(mesh, b.material)
        base.append(off)
        for v in b.verts:
            pos += struct.pack("<3f", *v["pos"])
            a = ([spm.half_to_float(v["uv"][0]), spm.half_to_float(v["uv"][1])]
                 if uv1 else [0.0, 0.0])
            if mesh.read_normal:
                a += spm.unpack_normal(v["normal"])
            att += struct.pack("<%df" % len(a), *a)
            owner.append(bi)
        all_idx += [i + off for i in b.indices]
        off += len(b.verts)
    # Prune deletes small disconnected components outright. Fine for
    # decoration; never for karts (whose body parts are separate pieces) or
    # the track model (a floating platform is also a separate piece).
    options = (1 if lock_border else 0) | (PRUNE if allow_sloppy else 0)
    if road is None:
        err = 1.0
        if max_error_m is not None:
            bb = mesh.bbox
            extent = max(bb[3] - bb[0], bb[4] - bb[1], bb[5] - bb[2], 1e-3)
            err = max_error_m / extent
        out = _run_meshsimplify(pos, att, weights, all_idx, off, target_tris,
                                options, target_error=err)
    else:
        # Driveable surface first-class: pass 1 freezes every vertex on the
        # road and spends the reduction on scenery; pass 2 lets the road go
        # too, but only where it is flat to within road["error"] metres, so
        # the surface the wheels raycast against keeps its shape.
        # Three zones around the driveline: far scenery (free, small
        # disconnected bits may be pruned), near-road (walls, barriers,
        # kerbs: shape kept to road["near_error"] m) and the road itself
        # (road["error"] m). Each pass locks the zones it must not touch.
        bb = mesh.bbox
        extent = max(bb[3] - bb[0], bb[4] - bb[1], bb[5] - bb[2], 1e-3)
        road_m = road["mask"](pos, off)
        near_m = road["near_mask"](pos, off)
        lock_near = bytes(a | b for a, b in zip(road_m, near_m))
        out = _run_meshsimplify(pos, att, weights, all_idx, off, target_tris,
                                options | PRUNE, lock=lock_near,
                                target_error=road["far_error"])
        out = _run_meshsimplify(pos, att, weights, out, off, 0, options,
                                lock=road_m,
                                target_error=road["near_error"] / extent)
        out = _run_meshsimplify(pos, att, weights, out, off, 0, options,
                                target_error=road["error"] / extent)
    per_buf = [[] for _ in bufs]
    for t in range(0, len(out), 3):
        a, b_, c = out[t:t + 3]
        o = owner[a]
        if owner[b_] != o or owner[c] != o:
            return mesh                      # would mix buffers: keep as is
        per_buf[o] += [a - base[o], b_ - base[o], c - base[o]]
    if allow_sloppy and len(out) // 3 > target_tris * 1.3:
        # Topology-preserving collapse stalled (many disconnected pieces:
        # grids, foliage cards, debris). Cluster each buffer on its own --
        # never across buffers, so materials stay put.
        got = len(out) // 3
        for bi, b in enumerate(bufs):
            n = len(per_buf[bi]) // 3
            if n < 8:
                continue
            want = max(4, int(n * target_tris / got))
            bpos = pos[base[bi] * 12:(base[bi] + len(b.verts)) * 12]
            idx = _run_meshsimplify(bpos, b"", [], per_buf[bi], len(b.verts),
                                    want, SLOPPY)
            if len(idx) >= 3:
                per_buf[bi] = idx
    for bi, b in enumerate(bufs):
        if len(per_buf[bi]) < 3:
            # meshopt removed a whole material buffer (a sliver, usually).
            # Keep a single triangle-free buffer out of the file instead of an
            # empty one, which the loader asserts on.
            per_buf[bi] = None
            continue
        _compact(b, per_buf[bi])
    for sec in mesh.sections:
        sec[:] = [b for b, k in zip(sec, [per_buf[bufs.index(x)] for x in sec])
                  if k is not None]
    mesh.sections = [sec for sec in mesh.sections if sec]
    spm.recompute_bbox(mesh)
    return mesh


def load_quads(track_dir):
    """Driveline quads from quads.xml as a list of 4x3 corner arrays."""
    import xml.etree.ElementTree as ET
    path = os.path.join(track_dir, "quads.xml")
    if not os.path.isfile(path):
        return [], (-1.0, 5.0)
    root = ET.parse(path).getroot()
    ht = root.find("height-testing")
    hmin, hmax = ((float(ht.get("min", -1)), float(ht.get("max", 5)))
                  if ht is not None else (-1.0, 5.0))
    quads = []
    for q in root.iter("quad"):
        pts = []
        for k in ("p0", "p1", "p2", "p3"):
            v = q.get(k, "")
            if ":" in v:
                qi, pi = v.split(":")
                pts.append(quads[int(qi)][int(pi)])
            else:
                pts.append([float(x) for x in v.split()])
        quads.append(pts)
    return quads, (hmin, hmax)


def make_road_mask(quads, heights, margin=2.0, above=1.5, below=1.0):
    """Vertex -> 1 if it lies on (or within `margin` m of) a driveline quad,
    in plan view, and inside the quad's height-testing window."""
    import numpy as np
    hmin, hmax = heights
    Q = np.array(quads, dtype=np.float64) if quads else None

    def mask(pos_bytes, vc):
        P = np.frombuffer(bytes(pos_bytes), dtype="<f4").reshape(vc, 3)
        out = np.zeros(vc, dtype=np.uint8)
        if Q is None:
            return out.tobytes()
        x, y, z = P[:, 0], P[:, 1], P[:, 2]
        for quad in Q:
            qx, qy, qz = quad[:, 0], quad[:, 1], quad[:, 2]
            sel = ((x >= qx.min() - margin) & (x <= qx.max() + margin) &
                   (z >= qz.min() - margin) & (z <= qz.max() + margin) &
                   (y >= qy.min() + hmin - below) & (y <= qy.max() + above))
            if not sel.any():
                continue
            ix = np.nonzero(sel)[0]
            px, pz = x[ix], z[ix]
            # inside test against the quad's edges (convex, either winding),
            # widened by `margin`
            signs = []
            for i in range(4):
                ax, az = qx[i], qz[i]
                bx, bz = qx[(i + 1) % 4], qz[(i + 1) % 4]
                ex, ez = bx - ax, bz - az
                ln = max(np.hypot(ex, ez), 1e-6)
                signs.append(((px - ax) * ez - (pz - az) * ex) / ln)
            S = np.stack(signs)
            inside = (S <= margin).all(axis=0) | (S >= -margin).all(axis=0)
            out[ix[inside]] = 1
        return out.tobytes()
    return mask


def _compact(b, new_idx):
    """Drop unreferenced vertices, keeping first-use order."""
    used = sorted(set(new_idx))
    remap = {old: i for i, old in enumerate(used)}
    b.verts = [b.verts[i] for i in used]
    b.indices = [remap[i] for i in new_idx]


# ----------------------------------------------------------------------------
# Textures
# ----------------------------------------------------------------------------
def _pow2_floor(x):
    p = 1
    while p * 2 <= x:
        p *= 2
    return p


def downscale_image(src, dst, texels, max_edge):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    im = Image.open(src)
    im.load()
    w, h = im.size
    # keep aspect, power-of-two edges (the RDP wraps textures by masking),
    # no larger than the source
    scale = min(1.0, (texels / float(w * h)) ** 0.5)
    nw = min(max_edge, max(4, _pow2_floor(max(1, int(w * scale)))))
    nh = min(max_edge, max(4, _pow2_floor(max(1, int(h * scale)))))
    while nw * nh > texels:
        if nw >= nh:
            nw //= 2
        else:
            nh //= 2
    if im.mode not in ("RGB", "RGBA", "L", "LA"):
        im = im.convert("RGBA" if "A" in im.getbands() or
                        im.info.get("transparency") is not None else "RGB")
    out = im.resize((nw, nh), Image.LANCZOS)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    ext = os.path.splitext(dst)[1].lower()
    if ext in (".jpg", ".jpeg"):
        out.convert("RGB").save(dst, quality=92)
    else:
        out.save(dst, optimize=True)
    return (w, h), (nw, nh)


# ----------------------------------------------------------------------------
# Tree walk
# ----------------------------------------------------------------------------
class Pipeline:
    def __init__(self, assets, out, budgets, log):
        self.assets = os.path.abspath(assets)
        self.out = os.path.abspath(out)
        self.b = budgets
        self.log = log
        self.mesh_cache = {}
        self.done_meshes = {}          # src path -> output tri count
        self.done_images = {}
        self.report = {"tracks": {}, "karts": {}, "totals": {}}

    def outpath(self, src):
        return os.path.join(self.out, os.path.relpath(src, self.assets))

    def load(self, p):
        if p not in self.mesh_cache:
            with open(p, "rb") as f:
                self.mesh_cache[p] = spm.load(f.read())
        return self.mesh_cache[p]

    def write_mesh(self, src, mesh):
        dst = self.outpath(src)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(spm.dump(mesh))

    def copy_tree_non_assets(self, src_dir):
        """Copy XML and other small data verbatim; meshes/images handled
        separately; music/sfx untouched here."""
        for root, _dirs, files in os.walk(src_dir):
            for fn in files:
                low = fn.lower()
                if low.endswith(".spm") or low.endswith(IMAGE_EXT):
                    continue
                if low.endswith((".xcf", ".blend", ".psd", ".svg")):
                    continue
                s = os.path.join(root, fn)
                d = self.outpath(s)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)

    # -- shared meshes ------------------------------------------------------
    def shared_mesh(self, p, cap, collides=False):
        """library/ and models/ meshes: decimated once, independent of the
        track that uses them."""
        if p in self.done_meshes:
            return self.done_meshes[p]
        m = self.load(p)
        target = max(min(m.tri_count, self.b["min_mesh_tris"]), cap)
        if m.tri_count > target:
            if collides:
                m = decimate(m, target,
                             max_error_m=self.b["collision_error_m"])
            else:
                m = decimate(m, target, allow_sloppy=True)
        self.write_mesh(p, m)
        self.done_meshes[p] = m.tri_count
        return m.tri_count

    # -- images -----------------------------------------------------------------
    def image(self, src, texels):
        if src in self.done_images:
            return self.done_images[src]
        dst = self.outpath(src)
        try:
            old, new = downscale_image(src, dst, texels,
                                       self.b["texture_max_edge"])
        except Exception as e:           # unreadable/odd image: copy as is
            self.log("  ! image %s: %s" % (src, e))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            old = new = (0, 0)
        self.done_images[src] = (old, new)
        return old, new

    # -- tracks -------------------------------------------------------------------
    def track(self, td):
        name = os.path.basename(td.rstrip("/"))
        info = stk_scene.track_info(td)
        usage = stk_scene.resolve_track(td, self.assets)
        coarse = stk_scene.lowest_lod_map(usage)
        self.copy_tree_non_assets(td)

        local, shared = {}, {}
        for p, c in usage.instances.items():
            (local if p.startswith(os.path.abspath(td) + os.sep) else shared)[p] = c

        # library objects this track places: their node.xml and images too
        for lib in usage.libraries:
            lib_dir = os.path.join(self.assets, "library", lib)
            if os.path.isdir(lib_dir):
                self.copy_tree_non_assets(lib_dir)
                for r, _d, fs in os.walk(lib_dir):
                    for f in fs:
                        if f.lower().endswith(IMAGE_EXT):
                            self.image(os.path.join(r, f),
                                       self.b["texture_texels"])

        # shared meshes first (fixed cost for this track)
        shared_unique = shared_scene = 0
        for p, c in shared.items():
            if p.startswith(os.path.join(self.assets, "models")):
                cap = self.b["model_tris"]
            else:
                cap = max(self.b["min_mesh_tris"],
                          int(self.load(p).tri_count * self.b["library_ratio"]))
            t = self.shared_mesh(p, cap, usage.collides.get(p, False))
            shared_unique += t
            if p not in coarse:
                shared_scene += t * c

        # local meshes: one ratio for the whole track, found by bisection on
        # the budgets. LOD: finer levels become copies of the coarsest.
        orig = {p: self.load(p).tri_count for p in local}
        lo, hi = 0.0, 1.0
        u_budget = max(0, self.b["track_unique_tris"] - shared_unique)
        s_budget = max(0, self.b["track_scene_tris"] - shared_scene)

        def cost(r):
            u = s = 0
            for p, c in local.items():
                src = coarse.get(p, p)
                t = max(min(orig[src], self.b["min_mesh_tris"]),
                        int(orig[src] * r))
                if p not in coarse:
                    u += t
                    s += t * c
            return u, s

        for _ in range(30):
            mid = (lo + hi) / 2
            u, s = cost(mid)
            if u <= u_budget and s <= s_budget:
                lo = mid
            else:
                hi = mid
        ratio = lo
        self.log("track %-28s ratio %.4f" % (name, ratio))

        quads, heights = load_quads(td)
        road = {"mask": make_road_mask(quads, heights),
                "near_mask": make_road_mask(quads, heights,
                                            margin=self.b["near_road_m"],
                                            above=15.0, below=8.0),
                "error": self.b["road_error_m"],
                "near_error": self.b["collision_error_m"],
                "far_error": self.b["far_prune_error"]} if quads else None
        main_model = self._main_model(td)
        out_unique = out_scene = 0
        for p, c in local.items():
            src = coarse.get(p, p)
            m = spm.load(spm.dump(self.load(src)))     # private copy
            target = max(min(m.tri_count, self.b["min_mesh_tris"]),
                         int(m.tri_count * ratio))
            m = self._decimate_placed(m, target, p, usage, road, main_model)
            self.write_mesh(p, m)
            if p not in coarse:
                out_unique += m.tri_count
                out_scene += m.tri_count * c

        # Meshes in the track directory that no placement instantiates (a LOD
        # group nobody instances, leftovers) are still loaded by the game when
        # their group is defined, so they must exist: same ratio.
        for root, _d, files in os.walk(td):
            for fn in files:
                p = os.path.join(root, fn)
                if fn.endswith(".spm") and p not in local:
                    m = spm.load(spm.dump(self.load(p)))
                    target = max(min(m.tri_count, self.b["min_mesh_tris"]),
                                 int(m.tri_count * ratio))
                    self.write_mesh(p, decimate(m, target, allow_sloppy=True))

        # textures used by any mesh of this track + everything in the track dir
        tex_names = set()
        for p in usage.instances:
            for t1, t2 in self.load(p).materials:
                tex_names.update(x for x in (t1, t2) if x)
        crowded = len(tex_names) > self.b["crowded_texture_count"]
        texels = self.b["texture_texels_crowded" if crowded else
                        "texture_texels"]
        tex_bytes = 0
        for root, _d, files in os.walk(td):
            for fn in files:
                if fn.lower().endswith(IMAGE_EXT):
                    _o, (w, h) = self.image(os.path.join(root, fn), texels)
                    tex_bytes += w * h // 2          # CI4 estimate
        for t in tex_names:
            g = os.path.join(self.assets, "textures", t)
            if os.path.isfile(g):
                _o, (w, h) = self.image(g, texels)
                tex_bytes += w * h // 2

        self.report["tracks"][name] = {
            "kind": info.get("kind", "?"),
            "src_unique_tris": sum(self.load(p).tri_count
                                   for p in usage.instances),
            "unique_tris": out_unique + shared_unique,
            "scene_tris": out_scene + shared_scene,
            "local_ratio": round(ratio, 5),
            "textures": len(tex_names),
            "texture_bytes_ci4": tex_bytes,
            "missing": sorted(usage.missing),
        }

    def _decimate_placed(self, m, target, p, usage, road, main_model):
        if p == main_model:
            return decimate(m, target, road=road)
        if p in usage.driveable or usage.collides.get(p, False):
            # karts touch these triangles: topology-preserving only, shape
            # error capped (driveable surfaces at road accuracy)
            err = (self.b["road_error_m"] if p in usage.driveable
                   else self.b["collision_error_m"])
            return decimate(m, target, max_error_m=err)
        return decimate(m, target, allow_sloppy=True)

    def _main_model(self, td):
        import xml.etree.ElementTree as ET
        try:
            root = ET.parse(os.path.join(td, "scene.xml")).getroot()
        except (ET.ParseError, FileNotFoundError):
            return None
        t = root.find("track")
        if t is None or not t.get("model"):
            return None
        return os.path.normpath(os.path.join(td, t.get("model")))

    # -- karts ----------------------------------------------------------------------
    def kart(self, kd):
        name = os.path.basename(kd.rstrip("/"))
        self.copy_tree_non_assets(kd)
        meshes = [os.path.join(r, f) for r, _d, fs in os.walk(kd)
                  for f in fs if f.endswith(".spm")]
        src_total = sum(self.load(p).tri_count for p in meshes)
        ratio = min(1.0, self.b["kart_tris"] / max(1, src_total))
        out_total = 0
        for p in meshes:
            m = spm.load(spm.dump(self.load(p)))
            target = max(min(m.tri_count, self.b["min_mesh_tris"]),
                         int(m.tri_count * ratio))
            # karts are closed, separately-skinned parts: no border locking
            m = decimate(m, target, lock_border=False)
            self.write_mesh(p, m)
            out_total += m.tri_count
        tex_bytes = 0
        for r, _d, fs in os.walk(kd):
            for f in fs:
                if f.lower().endswith(IMAGE_EXT):
                    texels = (64 * 64 if f.startswith("icon") or "icon" in f
                              else self.b["texture_texels"])
                    _o, (w, h) = self.image(os.path.join(r, f), texels)
                    tex_bytes += w * h // 2
        self.report["karts"][name] = {"src_tris": src_total,
                                      "tris": out_total,
                                      "texture_bytes_ci4": tex_bytes}

    # -- the rest -------------------------------------------------------------------
    def models(self):
        md = os.path.join(self.assets, "models")
        self.copy_tree_non_assets(md)
        for r, _d, fs in os.walk(md):
            for f in fs:
                p = os.path.join(r, f)
                if f.endswith(".spm"):
                    self.shared_mesh(p, self.b["model_tris"])
                elif f.lower().endswith(IMAGE_EXT):
                    self.image(p, self.b["texture_texels"])

    def textures_dir(self):
        td = os.path.join(self.assets, "textures")
        self.copy_tree_non_assets(td)
        for r, _d, fs in os.walk(td):
            for f in fs:
                if f.lower().endswith(IMAGE_EXT):
                    self.image(os.path.join(r, f), self.b["texture_texels"])

    def library_rest(self):
        """Library objects no track referenced still get converted, so the
        tree stays complete (addon tracks may use them)."""
        ld = os.path.join(self.assets, "library")
        self.copy_tree_non_assets(ld)
        for r, _d, fs in os.walk(ld):
            for f in fs:
                p = os.path.join(r, f)
                if f.endswith(".spm") and p not in self.done_meshes:
                    cap = max(self.b["min_mesh_tris"],
                              int(self.load(p).tri_count *
                                  self.b["library_ratio"]))
                    self.shared_mesh(p, cap)
                elif f.lower().endswith(IMAGE_EXT):
                    self.image(p, self.b["texture_texels"])

    def copy_audio(self):
        for d in ("music", "sfx"):
            s = os.path.join(self.assets, d)
            if os.path.isdir(s):
                shutil.copytree(s, os.path.join(self.out, d),
                                dirs_exist_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--assets", required=True, help="stk-assets checkout")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tracks", help="comma list (default: all)")
    ap.add_argument("--karts", help="comma list (default: all)")
    ap.add_argument("--budget", action="append", default=[],
                    metavar="KEY=VALUE", help="override a BUDGETS entry")
    ap.add_argument("--no-audio", action="store_true")
    a = ap.parse_args()
    b = dict(BUDGETS)
    for kv in a.budget:
        k, v = kv.split("=", 1)
        if k not in b:
            ap.error("unknown budget key %s" % k)
        b[k] = type(b[k])(v)
    pipe = Pipeline(a.assets, a.out, b, lambda s: print(s, flush=True))
    os.makedirs(pipe.out, exist_ok=True)

    tracks = sorted(os.listdir(os.path.join(pipe.assets, "tracks")))
    if a.tracks:
        tracks = a.tracks.split(",")
    karts = sorted(os.listdir(os.path.join(pipe.assets, "karts")))
    if a.karts:
        karts = a.karts.split(",")

    pipe.models()
    pipe.textures_dir()
    for t in tracks:
        pipe.track(os.path.join(pipe.assets, "tracks", t))
    for k in karts:
        pipe.kart(os.path.join(pipe.assets, "karts", k))
    if not a.tracks:
        pipe.library_rest()
    if not a.no_audio:
        pipe.copy_audio()

    pipe.report["budgets"] = b
    with open(os.path.join(pipe.out, "n64_budget_report.json"), "w") as f:
        json.dump(pipe.report, f, indent=1, sort_keys=True)
    print("report: %s" % os.path.join(pipe.out, "n64_budget_report.json"))


if __name__ == "__main__":
    main()
