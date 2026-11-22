"""Resolve what a SuperTuxKart track actually instantiates.

Follows scene.xml the way src/tracks/track.cpp and track_object.cpp do:
the main <track model>, <static-object>/<object> models, LOD groups
(<lod><group> definitions instanced with lod_instance="true"), and
<library name=...> placements, which load library/<name>/node.xml recursively
with the library's own directory as the model search path.

The result is a list of (mesh_path, instance_count, lod_levels) so callers can
reason about unique memory (each mesh loaded once) versus drawn geometry
(instances x triangles).
"""
import os
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict


def _parse(path):
    try:
        return ET.parse(path).getroot()
    except (ET.ParseError, FileNotFoundError):
        return None


def _find_model(name, search_dirs):
    for d in search_dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return os.path.normpath(p)
    return None


class SceneUsage:
    def __init__(self):
        self.instances = Counter()         # mesh path -> instance count
        self.lod_groups = {}               # group name -> [(distance, path)]
        self.missing = set()
        self.libraries = Counter()
        # mesh path -> True if any placement makes it a *mesh* collider (the
        # rules of TrackObject::init / PhysicalObject::fromXML: anything not
        # ghost/none collides; box/sphere/cylinder/cone shapes collide as
        # primitives, so only "exact" -- or static geometry merged into the
        # track, which has no shape -- uses the triangles themselves)
        self.collides = {}
        self.driveable = set()


def resolve_track(track_dir, assets_root):
    usage = SceneUsage()
    search = [track_dir, os.path.join(assets_root, "models")]
    root = _parse(os.path.join(track_dir, "scene.xml"))
    if root is None:
        return usage
    _walk(root, search, assets_root, usage, 1, depth=0)
    return usage


PRIMITIVE_SHAPES = ("box", "sphere", "cylinder", "cylinderX", "cylinderY",
                    "cylinderZ", "cone", "coneX", "coneY", "coneZ")


def _mesh_collider(el):
    inter = el.get("interaction", "static")
    if inter in ("ghost", "none"):
        return False
    return el.get("shape") not in PRIMITIVE_SHAPES


def _note(usage, p, el):
    if el is not None:
        usage.collides[p] = usage.collides.get(p, False) or _mesh_collider(el)
        if el.get("driveable") in ("Y", "y", "true", "1"):
            usage.driveable.add(p)


def _add(usage, name, search, mult, el=None):
    p = _find_model(name, search)
    if p is None:
        usage.missing.add(name)
    else:
        usage.instances[p] += mult
        _note(usage, p, el)


def _walk(root, search, assets_root, usage, mult, depth):
    if depth > 8:
        return
    # LOD group definitions: every level's model is loaded; instances refer
    # to the group by name.
    for lod in root.iter("lod"):
        for g in lod.findall("group"):
            levels = []
            for so in g:
                model = so.get("model")
                if not model:
                    continue
                p = _find_model(model, search)
                if p is None:
                    usage.missing.add(model)
                    continue
                levels.append((float(so.get("lod_distance", "0")), p))
                _note(usage, p, so)
            usage.lod_groups[g.get("name")] = sorted(levels)
    for el in root.iter():
        tag = el.tag
        if tag == "track" and el.get("model"):
            _add(usage, el.get("model"), search, mult)
            p = _find_model(el.get("model"), search)
            if p:
                usage.collides[p] = True
                usage.driveable.add(p)
        elif tag in ("static-object", "object"):
            if el.get("lod_instance") == "true":
                grp = usage.lod_groups.get(el.get("lod_group"), [])
                for _dist, p in grp:
                    usage.instances[p] += mult
            elif el.get("model") and el.get("lod_group") is None:
                _add(usage, el.get("model"), search, mult, el)
            elif el.get("model") and not _in_lod_block(root, el):
                _add(usage, el.get("model"), search, mult, el)
        elif tag == "library":
            name = el.get("name")
            usage.libraries[name] += mult
            lib_dir = os.path.join(assets_root, "library", name)
            lib_root = _parse(os.path.join(lib_dir, "node.xml"))
            if lib_root is None:
                usage.missing.add("library:" + name)
                continue
            _walk(lib_root, [lib_dir, os.path.join(assets_root, "models")],
                  assets_root, usage, mult, depth + 1)


_lod_cache = {}


def _in_lod_block(root, el):
    key = id(root)
    if key not in _lod_cache:
        s = set()
        for lod in root.iter("lod"):
            for x in lod.iter():
                s.add(id(x))
        _lod_cache[key] = s
    return id(el) in _lod_cache[key]


def lowest_lod_map(usage):
    """mesh path -> replacement path using each LOD group's coarsest level."""
    out = {}
    for levels in usage.lod_groups.values():
        if len(levels) > 1:
            coarse = levels[-1][1]
            for _d, p in levels[:-1]:
                out[p] = coarse
    return out


def track_info(track_dir):
    root = _parse(os.path.join(track_dir, "track.xml"))
    if root is None:
        return {}
    t = root if root.tag == "track" else root.find("track")
    if t is None:
        return {}
    a = dict(t.attrib)
    kind = "race"
    if a.get("arena") == "Y":
        kind = "arena"
    if a.get("soccer") == "Y":
        kind = "soccer"
    if a.get("cutscene") == "Y":
        kind = "cutscene"
    if a.get("internal") == "Y":
        kind = "internal" if kind == "race" else kind
    a["kind"] = kind
    return a
