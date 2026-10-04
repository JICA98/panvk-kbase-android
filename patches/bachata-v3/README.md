# Bachata V3 series

Purpose: Bachata's PanVK V3 bundle (release 3.1.0): upstream g615-v11-csf-v0.1.0-beta.13 (universal v10-v14 ICD) plus the Bachata changes upstream does not carry.
Source/Reference: Bachata-S4-Dev runtime/patches/panvk-v3, packaged by runtime/scripts/package-panvk-v3.py
Base: Mesa 5a07217f034b3e50d8c7c7794f97a2df1742613b with `scripts/apply-patches.sh --profile g615-v11-csf` (upstream series sha256:b85576a0bebec3b56c9de083a6afd2ff8c26feafece2a844079b7bca254e3d9c)
Dependencies: apply in numeric order with `git am` after the upstream series
Build: Meson ARM64 glibc ICD, `-Dplatforms=x11,wayland -Dpanfrost-kmds=kbase,panthor -Dmesa-clc=system -Dprecomp-compiler=system`, host tools (mesa_clc, vtn_bindgen2, panfrost_compile) built from the same tree
Tested GPU: Mali-G615 MC6 (Poco X6 Pro) for 3.0.0; v10, v12, v13 and v14 paths built, not device tested

| Patch | Change |
|---|---|
| 0001 | Bachata composite delta: JM v9 (G57) support, CRC mapping and kbase fixes, compiler tweaks, robust SSBO vectorization always on for fragment shaders |
| 0002 | kbase timestamp frequency, utrace waits, ZS preload intersect |
| 0003 | kbase timestamp ns factor, arch-specific ZS preload intersect (v12 EARLY_ZS, v13 PREPASS) |
| 0004 | Faster software presentation for X11 |
| 0005 | Upstream GPU-side same-queue semaphore waits on by default (`PANVK_KBASE_GPU_SEMAPHORE_WAITS=0` restores CPU waits) |
| 0006 | Drop development hooks |
| 0007 | Report the Bachata V3.1 release in driverInfo (`PanVK-kbase beta.13 Bachata V3.1`) |

Upstream already carries the former V2 series patches for 4-byte Valhall storage buffer offsets, the cached memory budget query, the load/store vectorizer robustness bound, and the buffer-cache barrier change (csf-v11/058).
