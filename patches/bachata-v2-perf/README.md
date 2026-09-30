# Bachata V2 performance series

Purpose: PanVK kbase performance and G720 (v12) ZS preload support used by the Bachata PanVK V2 bundle (release 2.2.0).
Source/Reference: Bachata-S4-Dev runtime/patches/panvk-v2-perf
Tested GPU: Mali-G615 MC6 (Poco X6 Pro); v12 (G720) paths built and enabled for bundled MediaTek PanVK, not device tested
Tested Kbase UAPI: 1.21 (CSF)
Mesa base range: 5a07217f034b3e50d8c7c7794f97a2df1742613b plus the Bachata V2 composite (patch series sha256:5afebe6b73e4c57a386a2e175379d316f5030de655324b93f7faaaaf888ba5b9)
Dependencies: apply in numeric order on the V2 composite
Validation test: Dark Souls Remastered corridor, 1080p, same APK: V2 15.21 FPS, this series 19.28 FPS median; God of War III combat, 540p (Poco X6 Pro, Mali-G615): 0001-0007 26.9 FPS, with 0008 30.8-36.0 FPS median, 34-42 FPS median with the matching Bachata emulator changes (branch panvk/perf); a scripted fly-over cutscene still drops to 10-20 FPS

These patches are not yet rebased onto the sources.lock Mesa pin used by the other families.

## Release 2.2.0 (Bachata production bundle)

Archive `Bachata-PanVK-V2-g615-glibc.zip`, SHA-256 `a1c25dfbc785cde86c1dd621de6c9652a6185ededc7d2d66df9ce6a79e61764a`.
Library SHA-256 `75f0390e8211f37a8303605539b1b803db849ca9b7b43b6530886448e975a2cc` (patches 0001-0008, unchanged from 2.1.0-g615-g720-perf.2).
Tier by the release policy: `rc` (primary consumer, one device). G720 remains untested.

Regression pass on Poco X6 Pro with the matching Bachata emulator (150 s boot and play, median FPS of the second half):

| Game | Result |
|---|---|
| Sonic Mania | 60 FPS, in level |
| Brotato | 58 FPS, in run |
| Odin Sphere Leifthrasir | 55 FPS, in game |
| Dragon's Crown Pro | 52 FPS, character creation |
| Gravity Rush Remastered | 50 FPS, in game |
| God of War III Remastered | 40 FPS combat median (99 s), no visual glitches |
| Dark Souls Remastered | 32 FPS, in game |
| TMNT: Mutants in Manhattan | 30 FPS, in game |
| RESOGUN | exhausts device memory in its menu (403 MB buffer allocation fails); the pre-series build fails the same way |
