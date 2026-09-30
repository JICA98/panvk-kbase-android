# Bachata V2 performance series

Purpose: PanVK kbase performance and G720 (v12) ZS preload support used by the Bachata PanVK V2 2.1.0 bundle.
Source/Reference: Bachata-S4-Dev runtime/patches/panvk-v2-perf
Tested GPU: Mali-G615 MC6 (Poco X6 Pro); v12 (G720) paths built and enabled for bundled MediaTek PanVK, not device tested
Tested Kbase UAPI: 1.21 (CSF)
Mesa base range: 5a07217f034b3e50d8c7c7794f97a2df1742613b plus the Bachata V2 composite (patch series sha256:5afebe6b73e4c57a386a2e175379d316f5030de655324b93f7faaaaf888ba5b9)
Dependencies: apply in numeric order on the V2 composite
Validation test: Dark Souls Remastered corridor, 1080p, same APK: V2 15.21 FPS, this series 19.28 FPS median; God of War III combat, 540p (Poco X6 Pro, Mali-G615): 0001-0007 26.9 FPS, with 0008 30.8-36.0 FPS median, 34-42 FPS median with the matching Bachata emulator changes (branch panvk/perf); a scripted fly-over cutscene still drops to 10-20 FPS

These patches are not yet rebased onto the sources.lock Mesa pin used by the other families.
