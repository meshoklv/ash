"""Pose the player (c0000 + parts) with one frame of a DSR HKX animation, on the CPU at cook time.

Pure Python: soulstruct-havok decodes the spline-compressed clip (`to_interleaved_container`, no CompressAnim.exe).
Bones that the HKX skeleton lacks (weapon/twist/case bones that only exist in the FLVER) follow their FLVER parent
with their bind-pose local offset.
"""
from __future__ import annotations

import logging

import numpy as np

from dsrlib import DSR, Binder, binder_flver

logging.getLogger("soulstruct").setLevel(logging.ERROR)


def _entry(b: Binder, base: str):
    for e in b.entries:
        if e.name.replace("\\", "/").rsplit("/", 1)[-1].lower() == base.lower():
            return e
    raise KeyError(base)


def quat_to_mat(q) -> np.ndarray:
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], np.float64)


def affine(t, R) -> np.ndarray:
    M = np.eye(4)
    M[:3, :3] = R
    M[:3, 3] = t
    return M


# anim id "a00_0000" lives in c0000_a00_lo / _md / _hi / c0000_aNx anibnds
ANIBNDS = ["c0000_a00_lo", "c0000_a00_md", "c0000_a00_hi", "c0000_a0x", "c0000_a1x", "c0000_a2x", "c0000_a3x",
           "c0000_a4x", "c0000_a9x", "c0000"]


class CharPose:
    """Pose of one character: skeleton + clip from `<chr>.anibnd` (or the c0000 split anibnds), hierarchy and
    bind pose from the FLVER in `<chr>.chrbnd`."""

    def __init__(self, chr_id: str = "c0000", anim: str = "a00_0000", frame: int = 0):
        from soulstruct.havok.fromsoft.darksouls1r import AnimationHKX, SkeletonHKX
        sk = SkeletonHKX.from_bytes(_entry(Binder.from_path(DSR / "chr" / f"{chr_id}.anibnd.dcx"), "Skeleton.hkx"))
        hk_bones = sk.skeleton.bones
        anim_entry = None
        for name in (ANIBNDS if chr_id == "c0000" else [chr_id]):
            try:
                anim_entry = _entry(Binder.from_path(DSR / "chr" / f"{name}.anibnd.dcx"), f"{anim}.hkx")
                self.source = f"{name}/{anim}"
                break
            except (KeyError, FileNotFoundError):
                continue
        if anim_entry is None:
            raise KeyError(f"animation {anim} not found for {chr_id}")
        cont = AnimationHKX.from_bytes(anim_entry).animation_container
        if cont.is_spline:
            cont = cont.to_interleaved_container()
        self.frame_count = cont.frame_count
        frame = min(frame, cont.frame_count - 1)
        arm = cont.get_interleaved_data_in_armature_space(sk.skeleton)[frame]
        track_bones = cont.get_track_bone_indices()
        anim_mats = {}
        for ti, trs in enumerate(arm):
            bname = hk_bones[track_bones[ti]].name
            t = np.array(list(trs.translation), np.float64)
            q = np.array(trs.rotation._data if trs.rotation._data is not None else list(trs.rotation), np.float64)
            anim_mats[bname] = affine(t, quat_to_mat(q))
        # FLVER skeleton (c0000) gives hierarchy + bind pose for every bone the parts can reference
        f = binder_flver(Binder.from_path(DSR / "chr" / f"{chr_id}.chrbnd.dcx"))
        self.part_bone_names = [b.name for b in f.bones]
        names = [b.name for b in f.bones]
        bind = [affine(np.array(list(t)), np.array(r.data)) for t, r, s in f.get_bone_tree().get_bone_armature_space_transforms()]
        posed = [None] * len(names)

        def solve(i):
            if posed[i] is not None:
                return posed[i]
            n = names[i]
            if n in anim_mats:
                posed[i] = anim_mats[n]
            else:
                p = f.bones[i].parent_bone_index
                if p < 0:
                    posed[i] = bind[i]
                else:
                    local = np.linalg.inv(bind[p]) @ bind[i]
                    posed[i] = solve(p) @ local
            return posed[i]

        for i in range(len(names)):
            solve(i)
        self.names = names
        self.bind = {n: bind[i] for i, n in enumerate(names)}
        self.posed = {n: posed[i] for i, n in enumerate(names)}
        self.skin_mats = {n: self.posed[n] @ np.linalg.inv(self.bind[n]) for n in names}
        self.missing = [n for n in names if n not in anim_mats]

    def bone(self, name: str):
        """(t, R) posed armature transform, same shape as cook_scene.skeleton_c0000() entries."""
        M = self.posed[name]
        return M[:3, 3].astype(np.float32), M[:3, :3].astype(np.float32)

    def skin(self, d: dict, mesh) -> tuple[np.ndarray, np.ndarray]:
        """Linear-blend-skin one dynamic FLVER mesh (bind-pose vertices in armature space) into this pose.
        Needs the part FLVER's bone names: set `self.part_bone_names` before calling."""
        pos, nrm = d["pos"].astype(np.float64), d.get("normal", np.zeros_like(d["pos"])).astype(np.float64)
        bi = d["bone_indices"]
        bw = d.get("bone_weights")
        if bw is None:
            bw = np.zeros(bi.shape, np.float64); bw[:, 0] = 1
        lut = np.array(mesh.bone_indices) if len(mesh.bone_indices) else None
        out_p = np.zeros_like(pos)
        out_n = np.zeros_like(nrm)
        wsum = np.zeros(len(pos))
        for k in range(bi.shape[1]):
            w = bw[:, k]
            idx = bi[:, k]
            gidx = lut[np.clip(idx, 0, len(lut) - 1)] if lut is not None else idx
            for g in np.unique(gidx[w > 0]):
                sel = (gidx == g) & (w > 0)
                name = self.part_bone_names[g]
                S = self.skin_mats.get(name)
                if S is None:
                    S = np.eye(4)
                out_p[sel] += w[sel, None] * (pos[sel] @ S[:3, :3].T + S[:3, 3])
                out_n[sel] += w[sel, None] * (nrm[sel] @ S[:3, :3].T)
                wsum[sel] += w[sel]
        bad = wsum < 1e-6
        out_p[bad] = pos[bad]; out_n[bad] = nrm[bad]
        out_p[~bad] /= wsum[~bad, None]
        return out_p.astype(np.float32), out_n.astype(np.float32)


def KnightPose(anim: str = "a00_0000", frame: int = 0) -> CharPose:
    return CharPose("c0000", anim, frame)
