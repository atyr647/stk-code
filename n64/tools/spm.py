"""Reader/writer for SuperTuxKart .spm meshes (format version 1).

Mirrors src/graphics/sp_mesh_loader.cpp field for field. Vertices are kept as
parsed records so a decimated mesh can be written back with every surviving
vertex bit-identical to the original (UVs, colours, packed normals and
skinning data are never re-quantised).

Layout (all little-endian):
  "SP", u8 (version<<3 | type), u8 flags (bit0 normal, bit1 colour, bit2 tangent)
  f32 bbox[6]
  u16 material_count, then per material: u8 len + tex1, u8 len + tex2
  u16 section_count, then per section:
    u16 buffer_count, then per buffer:
      u32 vertex_count, u32 index_count, u16 material_id
      vertices, then indices (u16 if vertex_count > 255 else u8)
  type 1 (SPMA, animated): armature data follows; kept verbatim as `tail`.
"""
import struct

TYPE_NAMES = {0: "SPMS", 1: "SPMA", 2: "SPMN"}


class Buffer:
    __slots__ = ("material", "verts", "indices")

    def __init__(self, material, verts, indices):
        self.material = material      # index into Mesh.materials
        self.verts = verts            # list of dicts (see _read_vertex)
        self.indices = indices        # flat list of ints, 3 per triangle

    @property
    def tri_count(self):
        return len(self.indices) // 3


class Mesh:
    def __init__(self):
        self.version = 1
        self.type = 2
        self.read_normal = self.read_color = self.read_tangent = False
        self.bbox = [0.0] * 6
        self.materials = []           # list of (tex1, tex2)
        self.sections = []            # list of lists of Buffer
        self.tail = b""               # armature data for SPMA

    @property
    def skinned(self):
        return self.type == 1

    def buffers(self):
        for sec in self.sections:
            for b in sec:
                yield b

    @property
    def tri_count(self):
        return sum(b.tri_count for b in self.buffers())

    @property
    def vertex_count(self):
        return sum(len(b.verts) for b in self.buffers())


def _uv_flags(mesh, mat_id):
    t1, t2 = mesh.materials[mat_id]
    return bool(t1), bool(t2)


def load(data):
    m = Mesh()
    if data[:2] != b"SP":
        raise ValueError("not an spm file")
    byte = data[2]
    m.version = byte >> 3
    if m.version != 1:
        raise ValueError("unsupported spm version %d" % m.version)
    m.type = byte & ~0x08 & 0xFF
    flags = data[3]
    m.read_normal = bool(flags & 1)
    m.read_color = bool(flags >> 1 & 1)
    m.read_tangent = bool(flags >> 2 & 1)
    p = 4
    m.bbox = list(struct.unpack_from("<6f", data, p)); p += 24
    (nmat,) = struct.unpack_from("<H", data, p); p += 2
    for _ in range(nmat):
        texs = []
        for _k in range(2):
            n = data[p]; p += 1
            texs.append(data[p:p + n].decode("latin-1")); p += n
        m.materials.append(tuple(texs))
    (nsec,) = struct.unpack_from("<H", data, p); p += 2
    for _ in range(nsec):
        (nbuf,) = struct.unpack_from("<H", data, p); p += 2
        sec = []
        for _b in range(nbuf):
            vc, ic, mat = struct.unpack_from("<IIH", data, p); p += 10
            uv1, uv2 = _uv_flags(m, mat)
            verts = []
            for _v in range(vc):
                v, p = _read_vertex(m, data, p, uv1, uv2)
                verts.append(v)
            if vc > 255:
                idx = list(struct.unpack_from("<%dH" % ic, data, p)); p += 2 * ic
            else:
                idx = list(data[p:p + ic]); p += ic
            sec.append(Buffer(mat, verts, idx))
        m.sections.append(sec)
    m.tail = data[p:]
    return m


def _read_vertex(m, data, p, uv1, uv2):
    v = {"pos": struct.unpack_from("<3f", data, p)}; p += 12
    if m.read_normal:
        v["normal"] = struct.unpack_from("<I", data, p)[0]; p += 4
    if m.read_color:
        ci = data[p]; p += 1
        if ci == 128:
            v["color"] = None          # "all white" marker
        else:
            v["color"] = (ci, data[p], data[p + 1], data[p + 2]); p += 3
    if uv1:
        v["uv"] = struct.unpack_from("<2h", data, p); p += 4
        if uv2:
            v["uv2"] = struct.unpack_from("<2h", data, p); p += 4
        if m.read_tangent:
            v["tangent"] = struct.unpack_from("<I", data, p)[0]; p += 4
    if m.skinned:
        v["joints"] = data[p:p + 16]; p += 16
    return v, p


def dump(m):
    out = bytearray(b"SP")
    out.append((m.version << 3) | m.type)
    out.append(int(m.read_normal) | int(m.read_color) << 1 |
               int(m.read_tangent) << 2)
    out += struct.pack("<6f", *m.bbox)
    out += struct.pack("<H", len(m.materials))
    for texs in m.materials:
        for t in texs:
            b = t.encode("latin-1")
            out.append(len(b)); out += b
    out += struct.pack("<H", len(m.sections))
    for sec in m.sections:
        out += struct.pack("<H", len(sec))
        for b in sec:
            vc, ic = len(b.verts), len(b.indices)
            out += struct.pack("<IIH", vc, ic, b.material)
            uv1, uv2 = _uv_flags(m, b.material)
            for v in b.verts:
                out += struct.pack("<3f", *v["pos"])
                if m.read_normal:
                    out += struct.pack("<I", v["normal"])
                if m.read_color:
                    c = v["color"]
                    if c is None:
                        out.append(128)
                    else:
                        out += bytes(c)
                if uv1:
                    out += struct.pack("<2h", *v["uv"])
                    if uv2:
                        out += struct.pack("<2h", *v["uv2"])
                    if m.read_tangent:
                        out += struct.pack("<I", v["tangent"])
                if m.skinned:
                    out += v["joints"]
            if vc > 255:
                out += struct.pack("<%dH" % ic, *b.indices)
            else:
                out += bytes(b.indices)
    out += m.tail
    return bytes(out)


def half_to_float(h):
    return struct.unpack("<e", struct.pack("<h", h))[0]


def unpack_normal(packed):
    """10:10:10:2 signed normal, as MiniGLM::decompressVector3 decodes it."""
    out = []
    for shift in (0, 10, 20):
        x = (packed >> shift) & 0x3FF
        if x & 0x200:
            x -= 0x400
        out.append(x / 511.0)
    return out


def recompute_bbox(m):
    xs, ys, zs = [], [], []
    for b in m.buffers():
        for i in set(b.indices):
            x, y, z = b.verts[i]["pos"]
            xs.append(x); ys.append(y); zs.append(z)
    if xs:
        m.bbox = [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]


# ---------------------------------------------------------------------------
# Skeletal animation (the `tail` of an SPMA mesh)
# ---------------------------------------------------------------------------
# Layout, as GE::Armature::read / SPMeshLoader::createAnimationData read it:
#   u8 armature_count, u16 bind_frame
#   per armature:
#     u16 joint_used, u16 joint_count, joint_count x (u8 len + name),
#     joint_count x LocRotScale (10 x f32: loc xyz, rot xyzw, scale xyz),
#     joint_count x i16 parent,
#     u16 frame_count, frame_count x (u16 frame_index,
#                                     joint_count x LocRotScale)
class Armature:
    def __init__(self):
        self.joint_used = 0
        self.names = []
        self.bind = []          # joint_count x 10 floats
        self.parents = []
        self.frames = []        # list of (frame_index, joint_count x 10 floats)


def load_animation(tail):
    p = 0
    n_arm = tail[p]; p += 1
    (bind_frame,) = struct.unpack_from("<H", tail, p); p += 2
    arms = []
    for _ in range(n_arm):
        a = Armature()
        a.joint_used, jc = struct.unpack_from("<HH", tail, p); p += 4
        for _j in range(jc):
            n = tail[p]; p += 1
            a.names.append(tail[p:p + n]); p += n
        for _j in range(jc):
            a.bind.append(struct.unpack_from("<10f", tail, p)); p += 40
        a.parents = list(struct.unpack_from("<%dh" % jc, tail, p)); p += 2 * jc
        (fc,) = struct.unpack_from("<H", tail, p); p += 2
        for _f in range(fc):
            (idx,) = struct.unpack_from("<H", tail, p); p += 2
            pose = [struct.unpack_from("<10f", tail, p + 40 * j) for j in range(jc)]
            p += 40 * jc
            a.frames.append((idx, pose))
        arms.append(a)
    return bind_frame, arms, tail[p:]


def dump_animation(bind_frame, arms, rest=b""):
    out = bytearray([len(arms)])
    out += struct.pack("<H", bind_frame)
    for a in arms:
        jc = len(a.names)
        out += struct.pack("<HH", a.joint_used, jc)
        for n in a.names:
            out.append(len(n)); out += n
        for b in a.bind:
            out += struct.pack("<10f", *b)
        out += struct.pack("<%dh" % jc, *a.parents)
        out += struct.pack("<H", len(a.frames))
        for idx, pose in a.frames:
            out += struct.pack("<H", idx)
            for lrs in pose:
                out += struct.pack("<10f", *lrs)
    return bytes(out) + rest


def _slerp(q1, q2, t):
    import math
    dot = sum(a * b for a, b in zip(q1, q2))
    if dot < 0:
        q2 = [-x for x in q2]; dot = -dot
    if dot > 0.9995:
        r = [a + t * (b - a) for a, b in zip(q1, q2)]
    else:
        th = math.acos(min(1.0, dot))
        s = math.sin(th)
        w1, w2 = math.sin((1 - t) * th) / s, math.sin(t * th) / s
        r = [w1 * a + w2 * b for a, b in zip(q1, q2)]
    n = math.sqrt(sum(x * x for x in r)) or 1.0
    return [x / n for x in r]


def _pose_error(a, b, interp_t, actual):
    """max (loc distance, rotation angle in rad, scale delta) between the
    interpolation of poses a->b at t and the actual pose, over all joints --
    the same lerp/slerp GE::Armature::getInterpolatedMatrices uses."""
    import math
    el = er = es = 0.0
    for ja, jb, jx in zip(a, b, actual):
        loc = [ja[i] + interp_t * (jb[i] - ja[i]) for i in range(3)]
        el = max(el, math.sqrt(sum((loc[i] - jx[i]) ** 2 for i in range(3))))
        q = _slerp(ja[3:7], jb[3:7], interp_t)
        qx = jx[3:7]
        n = math.sqrt(sum(v * v for v in qx)) or 1.0
        d = abs(sum(q[i] * qx[i] / n for i in range(4)))
        er = max(er, 2 * math.acos(min(1.0, d)))
        sc = [ja[7 + i] + interp_t * (jb[7 + i] - ja[7 + i]) for i in range(3)]
        es = max(es, max(abs(sc[i] - jx[7 + i]) for i in range(3)))
    return el, er, es


def reduce_keyframes(arm, keep, tol_loc=0.01, tol_rot=0.026, tol_scale=0.01):
    """Drop keyframes that interpolation between their neighbours reproduces
    within tolerance (default 1 cm, 1.5 degrees, 1 % scale). Frames whose
    index is in `keep` (animation start/end markers from kart.xml) and the
    first/last frames always stay. Greedy, left to right."""
    fr = arm.frames
    if len(fr) <= 2:
        return
    out = [fr[0]]
    anchor = 0
    j = 1
    while j < len(fr):
        if fr[j][0] in keep or j == len(fr) - 1:
            out.append(fr[j]); anchor = j; j += 1
            continue
        # can we skip fr[j] (and everything between anchor and j+1)?
        nxt = j + 1
        ok = True
        a_idx, a_pose = fr[anchor]
        b_idx, b_pose = fr[nxt]
        for k in range(anchor + 1, nxt):
            t = (fr[k][0] - a_idx) / float(b_idx - a_idx)
            el, er, es = _pose_error(a_pose, b_pose, t, fr[k][1])
            if el > tol_loc or er > tol_rot or es > tol_scale:
                ok = False
                break
        if ok:
            j += 1              # fr[j] is redundant; keep scanning
        else:
            out.append(fr[j]); anchor = j; j += 1
    arm.frames = out
