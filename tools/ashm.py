"""Mesh processing + the `.ashm` v1 writer (Ash static mesh/scene container for the PSP viewer).

Layout (little-endian, every section 16-byte aligned; offsets are from file start):

    Header (64 B)
      char  magic[4] = "ASHM"
      u32   version = 1
      u32   n_tex, n_batch, n_vert, n_idx, n_cam
      u32   flags (reserved)
      u32   off_tex, off_batch, off_vert, off_idx, off_cam
      f32   bbox_min[3]   (only 1 value used as pad -> keep 64 B: see HEADER_FMT)
    Tex    (16 B each): u16 w, u16 h, u16 psm(5 = T8), u16 flags(1 = alpha), u32 off_clut, u32 off_pix
                        CLUT = 256 x ABGR8888; pixels = w*h bytes, GE-swizzled (16 B x 8 rows blocks)
    Batch  (32 B each): u16 tex, u16 flags(1 = alpha test, 2 = no cull), u32 vert_start, u32 vert_count,
                        u32 idx_start, u32 idx_count, f32 center[3] ... (radius packed in last float)
    Vertex (24 B): f32 u, v; u32 color ABGR; f32 x, y, z      (GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF)
    Index  (2 B): u16, relative to the batch's vert_start
    Camera (32 B): f32 eye[3], f32 target[3], f32 fov_deg, f32 pad

Coordinates: FromSoft data is left-handed; the cooker mirrors X (x -> -x) so the PSP side is a plain
right-handed GL-style world. Front faces are CCW after cooking (mirroring flips the D3D CW winding).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

MAGIC = b"ASHM"
VERSION = 1
HEADER_FMT = "<4s12I4f"  # 4 + 48 + 16 = 68 -> padded to 80 below
HEADER_SIZE = 80
TEX_FMT = "<4H2I"
BATCH_FMT = "<2H4I4f"
VERT_DTYPE = np.dtype([("uv", "<f4", 2), ("color", "<u4"), ("pos", "<f4", 3)])
CAM_FMT = "<8f"

BF_ALPHA = 1
BF_NOCULL = 2


# ------------------------------------------------------------------ textures

def pow2_floor(n: int) -> int:
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def swizzle8(pix: np.ndarray) -> bytes:
    """GE swizzle for 8-bit textures: 16-byte x 8-row blocks, row-major blocks."""
    h, w = pix.shape
    assert w % 16 == 0 and h % 8 == 0, (w, h)
    blk = pix.reshape(h // 8, 8, w // 16, 16).transpose(0, 2, 1, 3)
    return np.ascontiguousarray(blk).tobytes()


@dataclass
class CookedTex:
    w: int
    h: int
    alpha: bool
    clut: bytes  # 1024 B
    pix: bytes   # swizzled
    src: tuple = ()


def cook_texture(im: Image.Image, max_size: int = 128, min_size: int = 16) -> CookedTex:
    """RGBA image -> pow2, <= max_size, CLUT8 (256 x 8888), binary alpha, swizzled."""
    w0, h0 = im.size
    w = max(min_size, min(max_size, pow2_floor(w0)))
    h = max(min_size, min(max_size, pow2_floor(h0)))
    # keep aspect ratio when clamping
    s = min(1.0, max_size / max(w0, h0))
    w = max(min_size, min(w, pow2_floor(max(1, int(w0 * s)))))
    h = max(min_size, min(h, pow2_floor(max(1, int(h0 * s)))))
    small = im.convert("RGBA").resize((w, h), Image.LANCZOS)
    a = np.asarray(small)
    alpha_mask = a[..., 3] < 128
    has_alpha = alpha_mask.mean() > 0.005
    rgb = Image.fromarray(np.ascontiguousarray(a[..., :3]))
    q = rgb.quantize(colors=255 if has_alpha else 256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    idx = np.asarray(q, dtype=np.uint8).copy()
    pal = np.array(q.getpalette()[: 256 * 3] + [0] * max(0, 768 - len(q.getpalette())), np.uint8).reshape(256, 3)
    clut = np.zeros((256, 4), np.uint8)
    clut[:, :3] = pal
    clut[:, 3] = 255
    if has_alpha:
        idx[alpha_mask] = 255
        clut[255] = (0, 0, 0, 0)
    return CookedTex(w, h, bool(has_alpha), clut.tobytes(), swizzle8(idx))


# ------------------------------------------------------------------ meshes

@dataclass
class Batch:
    """Unindexed-friendly working mesh: per-vertex arrays + triangle list."""
    tex: int
    pos: np.ndarray        # (N,3) f32
    uv: np.ndarray         # (N,2) f32
    col: np.ndarray        # (N,3) f32 linear 0..1
    tri: np.ndarray        # (M,3) int
    flags: int = 0
    name: str = ""
    attr: np.ndarray | None = None  # (N,K) optional extra per-vertex data (e.g. skin weights), carried like col

    @property
    def ntri(self):
        return len(self.tri)


def merge_batches(bs: list[Batch]) -> Batch:
    off = 0
    tris = []
    for b in bs:
        tris.append(b.tri + off)
        off += len(b.pos)
    attr = np.concatenate([b.attr for b in bs]) if bs[0].attr is not None else None
    return Batch(bs[0].tex, np.concatenate([b.pos for b in bs]), np.concatenate([b.uv for b in bs]),
                 np.concatenate([b.col for b in bs]), np.concatenate(tris), bs[0].flags, bs[0].name, attr)


def weld(b: Batch, eps=1e-4) -> Batch:
    """Merge vertices identical in pos/uv/col (quantized)."""
    parts = [np.round(b.pos / eps), np.round(b.uv / 1e-4), np.round(b.col * 255)]
    if b.attr is not None:
        parts.append(np.round(b.attr * 1024))
    key = np.concatenate(parts, 1).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    tri = inv[b.tri]
    keep = (tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2]) & (tri[:, 0] != tri[:, 2])
    return Batch(b.tex, b.pos[first], b.uv[first], b.col[first], tri[keep], b.flags, b.name,
                 None if b.attr is None else b.attr[first])


def simplify(b: Batch, target_tris: int) -> Batch:
    """Quadric simplification (pyfqmr) on positions, then UV/colour re-projected from the closest original
    triangle (barycentric, per new triangle, so UV seams do not smear across charts)."""
    if b.ntri <= target_tris or target_tris < 4:
        return b
    import pyfqmr
    # weld by position only for the topology the simplifier sees
    pkey = np.round(b.pos / 1e-4).astype(np.int64)
    _, pfirst, pinv = np.unique(pkey, axis=0, return_index=True, return_inverse=True)
    pinv = pinv.reshape(-1)
    s = pyfqmr.Simplify()
    s.setMesh(b.pos[pfirst].astype(np.float64), pinv[b.tri].astype(np.int32))
    s.simplify_mesh(target_count=int(target_tris), aggressiveness=7, preserve_border=True, verbose=False)
    nv, nf, _ = s.getMesh()
    nv = np.asarray(nv, np.float32)
    nf = np.asarray(nf, np.int64)
    if len(nf) == 0:
        return b
    # closest original triangle per new triangle (by centroid, refine among k candidates by plane distance)
    P = b.pos
    T = b.tri
    oc = P[T].mean(1)
    tree = cKDTree(oc)
    nc = nv[nf].mean(1)
    k = min(8, len(T))
    _, cand = tree.query(nc, k=k)
    cand = cand.reshape(len(nf), k)
    a, bb, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    on = np.cross(bb - a, c - a)
    onl = np.linalg.norm(on, axis=1, keepdims=True) + 1e-12
    on = on / onl
    nn = np.cross(nv[nf[:, 1]] - nv[nf[:, 0]], nv[nf[:, 2]] - nv[nf[:, 0]])
    nn = nn / (np.linalg.norm(nn, axis=1, keepdims=True) + 1e-12)
    # score: centroid distance + normal disagreement
    d = np.linalg.norm(oc[cand] - nc[:, None, :], axis=2)
    nd = 1.0 - np.einsum("ij,ikj->ik", nn, on[cand])
    best = cand[np.arange(len(nf)), np.argmin(d + nd * 0.5, axis=1)]
    # barycentric of each new corner w.r.t. chosen original triangle (projected onto its plane)
    corners = nv[nf]                       # (M,3,3)
    A, B, C = a[best], bb[best], c[best]   # (M,3)
    v0, v1 = B - A, C - A
    d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    v2 = corners - A[:, None, :]
    d20 = (v2 * v0[:, None, :]).sum(2); d21 = (v2 * v1[:, None, :]).sum(2)
    wv = (d11[:, None] * d20 - d01[:, None] * d21) / den[:, None]
    ww = (d00[:, None] * d21 - d01[:, None] * d20) / den[:, None]
    wu = 1 - wv - ww
    # clamp extrapolation for colour only (UV may extrapolate to stay affine)
    to = T[best]
    uvA, uvB, uvC = b.uv[to[:, 0]], b.uv[to[:, 1]], b.uv[to[:, 2]]
    uv = wu[..., None] * uvA[:, None] + wv[..., None] * uvB[:, None] + ww[..., None] * uvC[:, None]
    cw = np.clip(np.stack([wu, wv, ww], -1), 0, None)
    cw = cw / (cw.sum(-1, keepdims=True) + 1e-9)
    col = (cw[..., 0:1] * b.col[to[:, 0]][:, None] + cw[..., 1:2] * b.col[to[:, 1]][:, None]
           + cw[..., 2:3] * b.col[to[:, 2]][:, None])
    attr = None
    if b.attr is not None:
        attr = (cw[..., 0:1] * b.attr[to[:, 0]][:, None] + cw[..., 1:2] * b.attr[to[:, 1]][:, None]
                + cw[..., 2:3] * b.attr[to[:, 2]][:, None])
    # quadric collapses can flip faces; orient every new triangle like the original surface it came from
    flip = np.einsum("ij,ij->i", nn, on[best]) < 0
    corners[flip] = corners[flip][:, ::-1]
    uv[flip] = uv[flip][:, ::-1]
    col[flip] = col[flip][:, ::-1]
    if attr is not None:
        attr[flip] = attr[flip][:, ::-1]
        attr = attr.reshape(-1, attr.shape[-1]).astype(np.float32)
    M = len(nf)
    out = Batch(b.tex, corners.reshape(-1, 3), uv.reshape(-1, 2).astype(np.float32),
                col.reshape(-1, 3).astype(np.float32), np.arange(M * 3).reshape(M, 3), b.flags, b.name, attr)
    return weld(out)


def tessellate(b: Batch, max_edge: float) -> Batch:
    """Split triangles until every edge <= max_edge (PSP GE has no real clipping: big triangles near the camera
    get dropped). Longest-edge bisection on an unshared corner list, then weld."""
    P = b.pos[b.tri]; U = b.uv[b.tri]; Cc = b.col[b.tri]
    out_p, out_u, out_c = [], [], []
    for _ in range(12):
        e = np.stack([np.linalg.norm(P[:, 1] - P[:, 0], axis=1), np.linalg.norm(P[:, 2] - P[:, 1], axis=1),
                      np.linalg.norm(P[:, 0] - P[:, 2], axis=1)], 1)
        big = e.max(1) > max_edge
        out_p.append(P[~big]); out_u.append(U[~big]); out_c.append(Cc[~big])
        if not big.any():
            break
        P, U, Cc, e = P[big], U[big], Cc[big], e[big]
        k = e.argmax(1)
        # rotate so the longest edge is (0,1)
        idx = np.stack([k, (k + 1) % 3, (k + 2) % 3], 1)
        r = np.arange(len(P))[:, None]
        P, U, Cc = P[r, idx], U[r, idx], Cc[r, idx]
        mp, mu, mc = (P[:, 0] + P[:, 1]) / 2, (U[:, 0] + U[:, 1]) / 2, (Cc[:, 0] + Cc[:, 1]) / 2
        P = np.concatenate([np.stack([P[:, 0], mp, P[:, 2]], 1), np.stack([mp, P[:, 1], P[:, 2]], 1)])
        U = np.concatenate([np.stack([U[:, 0], mu, U[:, 2]], 1), np.stack([mu, U[:, 1], U[:, 2]], 1)])
        Cc = np.concatenate([np.stack([Cc[:, 0], mc, Cc[:, 2]], 1), np.stack([mc, Cc[:, 1], Cc[:, 2]], 1)])
    else:
        out_p.append(P); out_u.append(U); out_c.append(Cc)
    P = np.concatenate(out_p); U = np.concatenate(out_u); Cc = np.concatenate(out_c)
    M = len(P)
    return weld(Batch(b.tex, P.reshape(-1, 3), U.reshape(-1, 2), Cc.reshape(-1, 3), np.arange(M * 3).reshape(M, 3),
                      b.flags, b.name))


def split_for_u16(b: Batch, limit=65000) -> list[Batch]:
    if len(b.pos) <= limit:
        return [b]
    out = []
    step = max(1, len(b.tri) * limit // (len(b.pos) * 3 + 1))
    for i in range(0, len(b.tri), step):
        t = b.tri[i:i + step]
        used, inv = np.unique(t, return_inverse=True)
        out.append(Batch(b.tex, b.pos[used], b.uv[used], b.col[used], inv.reshape(-1, 3), b.flags, b.name))
    return out


# ------------------------------------------------------------------ writer

@dataclass
class Scene:
    textures: list[CookedTex] = field(default_factory=list)
    batches: list[Batch] = field(default_factory=list)
    cameras: list[tuple] = field(default_factory=list)  # (eye(3), target(3), fov)


def _align(buf: bytearray, a=16):
    while len(buf) % a:
        buf.append(0)


def write_ashm(path, sc: Scene) -> dict:
    bs = []
    for b in sc.batches:
        bs.extend(split_for_u16(b))
    body = bytearray(b"\0" * HEADER_SIZE)
    # textures: table then data
    off_tex = len(body)
    body += b"\0" * (16 * len(sc.textures)); _align(body)
    tex_entries = []
    for t in sc.textures:
        _align(body); oc = len(body); body += t.clut
        _align(body); op = len(body); body += t.pix
        tex_entries.append(struct.pack(TEX_FMT, t.w, t.h, 5, 1 if t.alpha else 0, oc, op))
    body[off_tex:off_tex + 16 * len(tex_entries)] = b"".join(tex_entries)
    _align(body)
    # vertices / indices
    verts, idxs, recs = [], [], []
    vs = is_ = 0
    for b in bs:
        v = np.zeros(len(b.pos), VERT_DTYPE)
        v["uv"] = b.uv
        c8 = np.clip(np.round(b.col * 255), 0, 255).astype(np.uint32)
        v["color"] = 0xFF000000 | (c8[:, 2] << 16) | (c8[:, 1] << 8) | c8[:, 0]
        v["pos"] = b.pos
        verts.append(v)
        idxs.append(b.tri.astype(np.uint16).reshape(-1))
        lo, hi = b.pos.min(0), b.pos.max(0)
        ctr = (lo + hi) / 2
        rad = float(np.linalg.norm(hi - lo) / 2)
        recs.append(struct.pack(BATCH_FMT, b.tex, b.flags, vs, len(b.pos), is_, b.tri.size, *ctr, rad))
        vs += len(b.pos); is_ += b.tri.size
    off_batch = len(body); body += b"".join(recs); _align(body)
    off_vert = len(body); body += b"".join(v.tobytes() for v in verts); _align(body)
    off_idx = len(body); body += b"".join(i.tobytes() for i in idxs); _align(body)
    off_cam = len(body)
    for eye, tgt, fov in sc.cameras:
        body += struct.pack(CAM_FMT, *eye, *tgt, fov, 0.0)
    _align(body)
    allp = np.concatenate([b.pos for b in bs])
    lo = allp.min(0)
    hdr = struct.pack(HEADER_FMT, MAGIC, VERSION, len(sc.textures), len(bs), vs, is_, len(sc.cameras), 0,
                      off_tex, off_batch, off_vert, off_idx, off_cam, *lo, 0.0)
    body[:len(hdr)] = hdr
    with open(path, "wb") as f:
        f.write(body)
    return dict(bytes=len(body), tex=len(sc.textures), batches=len(bs), verts=vs, tris=is_ // 3,
                tex_bytes=sum(len(t.clut) + len(t.pix) for t in sc.textures))
