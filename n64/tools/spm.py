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
