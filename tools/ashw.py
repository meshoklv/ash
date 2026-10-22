"""Zone package for the PSP: `.pak` container + `.ashw` world geometry + `.ashz` zone entities (tools/cook_world.py).

All little-endian, every section 16-aligned, offsets from the start of the blob. PSP space: x mirrored vs FromSoft
(the same convention as tools/ashm.py, tools/cook_collision.py), front faces CCW.

PAK v1 (`zone_<map>.pak`, read in one go, blobs 64-aligned)
    Header 16 B : "APAK", u32 version=1, u32 n_entries, u32 pad
    Entry  32 B : char name[16], u32 off, u32 size, u32 pad[2]          names: "world", "pcol", "ents"

ASHW v1 (world geometry)
    Header 96 B : "ASHW", u32 version=1, n_tex, n_batch, n_mesh, n_vert, n_idx, flags,
                  u32 off_tex, off_batch, off_mesh, off_vert, off_idx, pad[3], f32 bbox_min[3], bbox_max[3], chunk, pad
    Tex    16 B : u16 w, h, psm (4 = T4, 5 = T8), flags (1 = has transparent texels); u32 off_clut, off_pix
                  CLUT = 16 (T4) or 256 (T8) x ABGR8888; pixels GE-swizzled (16 B x 8 rows blocks), T4 low nibble first
    Batch  96 B : u16 tex, flags; u32 vert_start, vert_count, idx_start, idx_count;
                  f32 pos_off[3], pos_scale[3]   world = pos_off + s16/32768 * pos_scale   (= GE model matrix T*S)
                  f32 uv_off[2], uv_scale[2]     uv    = uv_off + u16/32768 * uv_scale     (= sceGuTexOffset/TexScale)
                  f32 center[3], radius          bounding sphere (world, or mesh-local for BF_MESH)
                  u32 mask[4]                    MSB draw groups (128 bits) of the source part: drawn when
                                                 mask & display_groups(collision under the player) != 0
                  u32 chunk                      (ix+512) | (iy+512)<<10 | (iz+512)<<20: LOD reference = centre of
                                                 grid cell (ix, iy, iz) of size header.chunk
    Mesh   16 B : u32 batch_start, batch_count; f32 radius; u32 pad      (object models, batches with BF_MESH)
    Vertex 12 B : u16 u, v; u16 color (RGB565: r 0-4, g 5-10, b 11-15); s16 x, y, z
                  = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D
    Index   2 B : u16 relative to the batch's vert_start

ASHZ v1 (zone entities, see ENT_* / ROLE_* / KIND_* below and src/world/world.h)
    Header 64 B : "ASHZ", u32 version=1, n_col, n_obj, n_box, n_chr, n_reg, start_reg,
                  u32 off_col, off_obj, off_box, off_chr, off_reg, pad[3]
    Col    64 B : char name[16]; u32 disp[4] (display groups: what is drawn while standing here); u32 draw[4]
                  (groups this collision belongs to); s32 cam_row; s32 sound_type; f32 reflect_y; u32 pad
    Obj    96 B : char name[16]; u32 entity; u16 kind; s16 mesh; u16 box_start, box_count; u32 flags;
                  f32 rot[9] (row-major 3x3, PSP space); f32 pos[3]; u32 mask[4]
    Box    32 B : f32 center[3], half[3] (object-local, PSP space), u32 flags, u32 pad
    Chr    64 B : char model[8]; u32 entity; f32 pos[3]; f32 yaw (PSP, radians, 0 = facing +z after mirror... see docs);
                  s32 npc_param, think_param, talk_id; s16 patrol[2] (region index, -1); u16 flags; u16 pad; u32 mask[4]
    Region 48 B : u32 entity; u16 shape (0 point, 1 sphere, 2 cylinder, 3 box); u16 role; f32 pos[3]; f32 yaw;
                  f32 size[3] (box w, h, d | sphere r | cylinder r, h); s32 param; u32 pad[2]
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np
from PIL import Image

from ashm import pow2_floor, swizzle8

# batch flags
BF_ALPHA = 1    # alpha test
BF_NOCULL = 2   # two-sided
BF_SKY = 4      # sky dome / far backdrop: drawn first, own far plane, no depth write, no fog
BF_FAR = 8      # distant terrain: never distance-culled, no fog
BF_MESH = 16    # object model batch (mesh-local), not part of the world pass
BF_ADD = 32     # additive blend (fog walls)
BF_LOD_NEAR = 64  # chunk LOD drawn when the chunk centre is nearer than the LOD distance (tessellated)
BF_LOD_FAR = 128  # chunk LOD drawn beyond it (coarser, not tessellated for close views)
BF_DECOR = 256    # small static props (jars, bags, gargoyles): own shorter cull distance
BF_BLEND = 512    # alpha blend (sky clouds): texture alpha from a CLUT with alpha levels, cook_texture_soft

# object kinds (KIND_*), roles of regions (ROLE_*), character flags
KINDS = dict(DECOR=0, BONFIRE=1, DOOR=2, CELL_DOOR=3, GATE=4, FOG=5, LADDER=6, FLOOR_BREAK=7, BOULDER=8,
             NEST=9, BREAKABLE=10)
ROLES = dict(NONE=0, START=1, RESPAWN=2, BOSS_ARENA=3, BOSS_START=4, BOSS_ENTRY=5, DEMON_DROP=6, DEMON_LAND=7,
             PLUNGE_ZONE=8, BOSS_ESCAPE=9, EXIT_CROW=10, CROW_NEST=11, MESSAGE=12, TORCH=13, FLOOR_COLLAPSE=14,
             BOULDER_TRIGGER=15, PATROL=16, BGM=17, TREASURE=18, FOG_VFX=19, KEY_DROP=20, LIGHT=21, OTHER=22,
             DEMON_BALCONY=23, PLUNGE_BALCONY_IN=24, CELL=25, INNER_DOOR=26)
CHR_BOSS = 1
CHR_CORE = 2   # suggested subset for the S1 slice (<= 8 hollows + boss)

SHAPES = dict(point=0, sphere=1, cylinder=2, box=3)


# ------------------------------------------------------------------ textures

@dataclass
class WTex:
    w: int
    h: int
    psm: int      # 4 = T4, 5 = T8
    alpha: bool
    clut: bytes
    pix: bytes
    src: tuple = ()


def cook_texture_soft(im: Image.Image, max_size=128, bits=4, min_w=32) -> WTex:
    """RGBA with soft alpha (clouds: white, alpha 5..227) -> CLUT4/8 whose palette keeps alpha (ABGR8888), for
    alpha-blended batches (BF_BLEND). Binary alpha + alpha test made the clouds noisy grey blotches (pt2-boss sky-noise)."""
    w0, h0 = im.size
    s = min(1.0, max_size / max(w0, h0))
    w = max(min_w, min(max_size, pow2_floor(max(1, int(w0 * s)))))
    h = max(8, min(max_size, pow2_floor(max(1, int(h0 * s)))))
    small = im.convert("RGBA").resize((w, h), Image.LANCZOS)
    ncl = 16 if bits == 4 else 256
    q = small.quantize(colors=ncl, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    idx = np.asarray(q, dtype=np.uint8).copy()
    pal = np.array(q.getpalette(rawmode="RGBA")[: ncl * 4], np.uint8)
    clut = np.zeros((ncl, 4), np.uint8)
    clut.reshape(-1)[: len(pal)] = pal
    if bits == 4:
        packed = (idx[:, 0::2] & 15) | ((idx[:, 1::2] & 15) << 4)
        return WTex(w, h, 4, True, clut.tobytes(), swizzle8(packed.astype(np.uint8)))
    return WTex(w, h, 5, True, clut.tobytes(), swizzle8(idx))


def cook_texture_world(im: Image.Image, max_size=128, bits=4, min_w=32) -> WTex:
    """RGBA -> pow2 <= max_size, CLUT4 (16 colours) or CLUT8, binary alpha (last entry transparent), swizzled."""
    w0, h0 = im.size
    s = min(1.0, max_size / max(w0, h0))
    w = max(min_w, min(max_size, pow2_floor(max(1, int(w0 * s)))))
    h = max(8, min(max_size, pow2_floor(max(1, int(h0 * s)))))
    small = im.convert("RGBA").resize((w, h), Image.LANCZOS)
    a = np.asarray(small)
    mask = a[..., 3] < 128
    has_alpha = bool(mask.mean() > 0.005)
    n = (16 if bits == 4 else 256) - (1 if has_alpha else 0)
    rgb = Image.fromarray(np.ascontiguousarray(a[..., :3]))
    q = rgb.quantize(colors=n, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.asarray(q, dtype=np.uint8).copy()
    ncl = 16 if bits == 4 else 256
    palette = q.getpalette()[: ncl * 3]
    pal = np.array(palette + [0] * (ncl * 3 - len(palette)), np.uint8).reshape(ncl, 3)
    clut = np.zeros((ncl, 4), np.uint8)
    clut[:, :3] = pal
    clut[:, 3] = 255
    if has_alpha:
        idx[mask] = ncl - 1
        clut[ncl - 1] = (0, 0, 0, 0)
    if bits == 4:
        packed = (idx[:, 0::2] & 15) | ((idx[:, 1::2] & 15) << 4)   # low nibble = even pixel
        pix = swizzle8(packed.astype(np.uint8))
        return WTex(w, h, 4, has_alpha, clut.tobytes(), pix)
    return WTex(w, h, 5, has_alpha, clut.tobytes(), swizzle8(idx))


# ------------------------------------------------------------------ geometry

VERT12 = np.dtype([("u", "<u2"), ("v", "<u2"), ("c", "<u2"), ("x", "<i2"), ("y", "<i2"), ("z", "<i2")])
assert VERT12.itemsize == 12


@dataclass
class WBatch:
    tex: int
    pos: np.ndarray     # (N,3) f32 (PSP space)
    uv: np.ndarray      # (N,2)
    col: np.ndarray     # (N,3) 0..1
    tri: np.ndarray     # (M,3)
    flags: int = 0
    mask: tuple = (0, 0, 0, 0)
    name: str = ""
    chunk: int = 0      # (ix+512) | (iy+512)<<10 | (iz+512)<<20 of the chunk grid cell (LOD reference point)


def mask128(bits) -> tuple:
    m = [0, 0, 0, 0]
    for b in bits:
        m[b // 32] |= 1 << (b % 32)
    return tuple(m)


def quantize_batch(b: WBatch):
    """-> (verts VERT12, pos_off, pos_scale, uv_off, uv_scale, center, radius)."""
    lo, hi = b.pos.min(0), b.pos.max(0)
    off = (lo + hi) / 2
    scale = np.maximum((hi - lo) / 2, 1e-3) * (32768 / 32767)
    q = np.clip(np.round((b.pos - off) / scale * 32768), -32767, 32767).astype(np.int16)
    ulo = np.floor(b.uv.min(0))
    rng = np.maximum(b.uv.max(0) - ulo, 1e-3)
    uvs = rng / 1.99          # u16 covers [0, 2)
    uq = np.clip(np.round((b.uv - ulo) / uvs * 32768), 0, 65535).astype(np.uint16)
    c = np.clip(b.col, 0, 1)
    r5 = np.round(c[:, 0] * 31).astype(np.uint16)
    g6 = np.round(c[:, 1] * 63).astype(np.uint16)
    b5 = np.round(c[:, 2] * 31).astype(np.uint16)
    v = np.zeros(len(b.pos), VERT12)
    v["u"], v["v"] = uq[:, 0], uq[:, 1]
    v["c"] = r5 | (g6 << 5) | (b5 << 11)
    v["x"], v["y"], v["z"] = q[:, 0], q[:, 1], q[:, 2]
    ctr = (lo + hi) / 2
    rad = float(np.linalg.norm(hi - lo) / 2)
    return v, off, scale, ulo, uvs, ctr, rad


def _align(buf: bytearray, a=16):
    while len(buf) % a:
        buf.append(0)


def write_ashw(textures: list[WTex], batches: list[WBatch], meshes: list[tuple[int, int, float]], chunk=12.0) -> tuple[bytes, dict]:
    """batches: world batches first, then mesh batches (meshes reference [start, count) in this list)."""
    body = bytearray(b"\0" * 96)
    off_tex = len(body)
    body += b"\0" * (16 * len(textures)); _align(body)
    recs = []
    for t in textures:
        _align(body); oc = len(body); body += t.clut
        _align(body); op = len(body); body += t.pix
        recs.append(struct.pack("<4H2I", t.w, t.h, t.psm, 1 if t.alpha else 0, oc, op))
    body[off_tex:off_tex + 16 * len(recs)] = b"".join(recs)
    _align(body)
    verts, idxs, brecs = [], [], []
    vs = is_ = 0
    for b in batches:
        assert len(b.pos) < 65536
        v, off, sc, uo, us, ctr, rad = quantize_batch(b)
        verts.append(v)
        idxs.append(b.tri.astype(np.uint16).reshape(-1))
        brecs.append(struct.pack("<2H4I3f3f2f2f4f4II", b.tex, b.flags, vs, len(v), is_, b.tri.size,
                                 *off, *sc, *uo, *us, *ctr, rad, *b.mask, b.chunk))
        vs += len(v); is_ += b.tri.size
    off_batch = len(body); body += b"".join(brecs); _align(body)
    off_mesh = len(body)
    for s, n, r in meshes:
        body += struct.pack("<2If4x", s, n, r)
    _align(body)
    off_vert = len(body); body += b"".join(v.tobytes() for v in verts); _align(body)
    off_idx = len(body); body += b"".join(i.tobytes() for i in idxs); _align(body)
    world = [b for b in batches if not b.flags & (BF_MESH | BF_SKY)]
    allp = np.concatenate([b.pos for b in world]) if world else np.zeros((1, 3))
    lo, hi = allp.min(0), allp.max(0)
    body[:96] = struct.pack("<4s7I8I3f3ff4x", b"ASHW", 1, len(textures), len(batches), len(meshes), vs, is_, 0,
                            off_tex, off_batch, off_mesh, off_vert, off_idx, 0, 0, 0, *lo, *hi, chunk)
    st = dict(bytes=len(body), tex=len(textures), tex_bytes=sum(len(t.clut) + len(t.pix) for t in textures),
              batches=len(batches), meshes=len(meshes), verts=vs, tris=is_ // 3,
              vert_bytes=vs * 12, idx_bytes=is_ * 2)
    return bytes(body), st


# ------------------------------------------------------------------ entities

@dataclass
class Zone:
    cols: list = field(default_factory=list)     # dict(name, disp(4), draw(4), cam_row, sound, reflect)
    objs: list = field(default_factory=list)     # dict(name, entity, kind, mesh, boxes[(c,h)], flags, rot(3x3), pos, mask)
    chrs: list = field(default_factory=list)     # dict(model, entity, pos, yaw, npc, think, talk, patrol(2), flags, mask)
    regs: list = field(default_factory=list)     # dict(entity, shape, role, pos, yaw, size(3), param)
    start_reg: int = 0


def _name16(s: str) -> bytes:
    b = s.encode("ascii", "replace")[:15]
    return b + b"\0" * (16 - len(b))


def write_ashz(z: Zone) -> bytes:
    body = bytearray(b"\0" * 64)
    off_col = len(body)
    for c in z.cols:
        body += _name16(c["name"]) + struct.pack("<4I4Iiif4x", *c["disp"], *c["draw"], c["cam_row"], c["sound"],
                                                 c["reflect"])
    _align(body)
    off_obj = len(body)
    boxes = []
    for o in z.objs:
        bs = len(boxes)
        boxes += o["boxes"]
        body += _name16(o["name"]) + struct.pack("<IHhHHI9f3f4I", o["entity"] & 0xFFFFFFFF, o["kind"], o["mesh"], bs,
                                                 len(o["boxes"]), o["flags"], *np.asarray(o["rot"]).reshape(9),
                                                 *o["pos"], *o["mask"])
    _align(body)
    off_box = len(body)
    for c, h, fl in boxes:
        body += struct.pack("<3f3fI4x", *c, *h, fl)
    _align(body)
    off_chr = len(body)
    for c in z.chrs:
        m = c["model"].encode()[:7]
        body += m + b"\0" * (8 - len(m)) + struct.pack("<I3ffiii2hHH4I", c["entity"] & 0xFFFFFFFF, *c["pos"], c["yaw"],
                                                       c["npc"], c["think"], c["talk"], *c["patrol"], c["flags"], 0,
                                                       *c["mask"])
    _align(body)
    off_reg = len(body)
    for r in z.regs:
        body += struct.pack("<IHH3ff3fi8x", r["entity"] & 0xFFFFFFFF, r["shape"], r["role"], *r["pos"], r["yaw"],
                            *r["size"], r["param"])
    _align(body)
    body[:64] = struct.pack("<4s7I5I12x", b"ASHZ", 1, len(z.cols), len(z.objs), len(boxes), len(z.chrs),
                            len(z.regs), z.start_reg, off_col, off_obj, off_box, off_chr, off_reg)
    return bytes(body)


def write_pak(entries: list[tuple[str, bytes]]) -> bytes:
    hdr = struct.pack("<4s3I", b"APAK", 1, len(entries), 0)
    table = bytearray()
    body = bytearray()
    base = 16 + 32 * len(entries)
    base = (base + 63) // 64 * 64
    for name, data in entries:
        while (base + len(body)) % 64:
            body.append(0)
        off = base + len(body)
        body += data
        table += _name16(name) + struct.pack("<2I8x", off, len(data))
    out = bytearray(hdr) + table
    while len(out) < base:
        out.append(0)
    return bytes(out + body)
