"""Shared helpers for reading DSR assets via Soulstruct (pure Python, no Blender).

Asset root: $ASH_DSR (default ~/ash-assets/dsr). Nothing read here is ever written into the repo.
"""
from __future__ import annotations

import io
import logging
import os
from functools import lru_cache
from pathlib import Path

import numpy as np

logging.getLogger("soulstruct").setLevel(logging.ERROR)

from PIL import Image  # noqa: E402
from soulstruct.containers import Binder, TPF  # noqa: E402
from soulstruct.flver import FLVER  # noqa: E402

DSR = Path(os.environ.get("ASH_DSR", Path.home() / "ash-assets/dsr"))


def tex_stem(path: str) -> str:
    """'N:\\...\\m18\\tex\\Foo_bar.tga' -> 'foo_bar'."""
    s = path.replace("\\", "/").rsplit("/", 1)[-1]
    for ext in (".dcx", ".tpf", ".tga", ".dds", ".psd"):
        if s.lower().endswith(ext):
            s = s[: -len(ext)]
    for ext in (".tpf", ".tga"):
        if s.lower().endswith(ext):
            s = s[: -len(ext)]
    return s.lower()


# ---------------------------------------------------------------- textures

@lru_cache(maxsize=None)
def _area_tex_index(area: str) -> dict[str, tuple[Path, int]]:
    """Index of map-area textures: stem -> (tpfbhd path, entry index)."""
    idx: dict[str, tuple[Path, int]] = {}
    for bhd in sorted((DSR / "map" / area).glob(f"{area}_*.tpfbhd")):
        b = Binder.from_path(bhd)
        for i, e in enumerate(b.entries):
            idx.setdefault(tex_stem(e.name), (bhd, i))
    return idx


@lru_cache(maxsize=8)
def _binder(path: Path) -> Binder:
    return Binder.from_path(path)


def dds_to_image(data: bytes) -> Image.Image:
    im = Image.open(io.BytesIO(data))
    im.load()
    return im.convert("RGBA")


def load_map_texture(area: str, stem: str) -> tuple[Image.Image, str] | None:
    """Returns (RGBA image, DDS fourcc) or None if missing."""
    hit = _area_tex_index(area).get(stem.lower())
    if hit is None:
        return None
    bhd, i = hit
    tpf = TPF.from_bytes(_binder(bhd).entries[i].data)
    tx = tpf.textures[0]
    return dds_to_image(tx.data), tx.data[84:88].decode("latin1")


def binder_textures(binder: Binder) -> dict[str, tuple[Image.Image, str]]:
    """All textures of every TPF inside a chrbnd/partsbnd: stem -> (RGBA image, fourcc)."""
    out = {}
    for e in binder.entries:
        if ".tpf" in e.name.lower():
            for tx in TPF.from_bytes(e.data).textures:
                try:
                    out[tx.stem.lower()] = (dds_to_image(tx.data), tx.data[84:88].decode("latin1"))
                except Exception as ex:  # pragma: no cover - report, keep going
                    print(f"  ! texture {tx.stem}: {ex}")
    return out


def binder_flver(binder: Binder) -> FLVER:
    for e in binder.entries:
        if e.name.lower().endswith(".flver"):
            return FLVER.from_bytes(e.data)
    raise FileNotFoundError("no .flver in binder")


# ---------------------------------------------------------------- meshes

def mesh_triangles(mesh) -> np.ndarray:
    """LOD0 triangle list (N,3) of vertex indices."""
    fs = mesh.face_sets[0]
    return fs.triangulate(uses_0xffff_separators=mesh.can_use_0xffff_separators).astype(np.int64)


def mesh_textures(mesh) -> dict[str, str]:
    """texture slot (g_Diffuse, g_Lightmap, ...) -> stem (first of each slot type)."""
    out = {}
    for t in mesh.material.textures:
        out.setdefault(t.texture_type, tex_stem(t.path))
    return out


def mesh_arrays(mesh) -> dict[str, np.ndarray]:
    v = mesh.vertices
    names = v.dtype.names
    d = {"pos": np.asarray(v["position"], np.float32)}
    if "normal" in names:
        d["normal"] = np.asarray(v["normal"], np.float32)
    if "uv_0" in names:
        d["uv0"] = np.asarray(v["uv_0"], np.float32)
    if "uv_1" in names:
        d["uv1"] = np.asarray(v["uv_1"], np.float32)
    if "color_0" in names:
        d["color"] = np.asarray(v["color_0"], np.float32)
    if "bone_indices" in names:
        d["bone_indices"] = np.asarray(v["bone_indices"], np.int32)
    if "bone_weights" in names:
        d["bone_weights"] = np.asarray(v["bone_weights"], np.float32)
    return d


def valid_meshes(f: FLVER) -> list:
    """Meshes whose vertex layout Soulstruct could decode (a few DSR map MTDs have unknown layouts)."""
    return [m for m in f.meshes if not getattr(m, "invalid_layout", False) and m.vertices.dtype.names]


def chr_textures(chr_id: str) -> dict[str, tuple[Image.Image, str]]:
    """Textures of a character: TPFs inside the chrbnd plus the split c####.chrtpfbhd/.chrtpfbdt pair
    (the BHD lives inside the chrbnd, the BDT next to it), for every chrbnd that has one (e.g. c2239 for c2230)."""
    import tempfile
    out = {}
    b = Binder.from_path(DSR / "chr" / f"{chr_id}.chrbnd.dcx")
    out.update(binder_textures(b))
    for e in b.entries:
        if e.name.lower().endswith(".chrtpfbhd"):
            bdt = DSR / "chr" / f"{chr_id}.chrtpfbdt"
            with tempfile.TemporaryDirectory() as td:
                bhd = Path(td) / f"{chr_id}.chrtpfbhd"
                bhd.write_bytes(e.data)
                out.update(binder_textures(Binder.from_path(bhd, bdt)))
    return out
