交付回执 — 轻量实机交付包（异机部署实机测试用）
================================================

生成时间: 2026-09-29
交付路径: D:\zzx\data\realtimeImaging\PAimage_CountBoundaryFrontendOnly_aa9097e_lightweight\
格式基准: PAimage_RingDisplay_c649a4e_lightweight（同目录既有交付包，文件集逐项对齐，
          仅运行期产物 app_log.txt / paimage-diagnostics/ 不随包）

交付身份
--------
分支:            codex/count-boundary-frontend-only-20260929-024946
实现 commit:     aa9097ef126ab3ea4a2a1ce2c5786ecaba7cd184（功能提交）
构建/交付 commit: 3c6323dcdf587322c896f8039868eca19ba5a3d5（分支 tip）
                 aa9097e 之后仅 CODEX_REPORTS 证据文档提交，生产与测试源码树
                 与 aa9097e 逐位一致；两个 exe 于 3c6323d 重新 configure+link
BuildIdentity:   PAIMAGE_GIT_SHA=3c6323d…、TRACKED_DIRTY=false、Debug、GNU 13.1.0
                 （主 exe 二进制内嵌字符串已实测核验；无旧 SHA 残留）
验证状态:        全量 CTest 51/51 —— aa9097e 首次全绿；3c6323d 交付树复验 51/51
                 （ctest_3c6323d.log 附后入档）

包内容（23 文件 + 7 个 Qt 插件目录，MANIFEST_SHA256.txt 全覆盖）
----------------------------------------------------------------
- PAimageReceiverDiagnostics.exe / ImagingSvc.exe（mingw-debug @3c6323d）
- Qt 6.8.0 运行库 + 插件（platforms/generic/iconengines/imageformats/
  networkinformation/styles/tls）+ MinGW 运行库（libgcc_s_seh/libstdc++/libwinpthread）
- MSVC 运行库三件（MSVCP140/VCRUNTIME140/VCRUNTIME140_1，取自 RingDisplay_c649a4e
  已实机验证包）
- CUDA/成像运行库：ring_recon_cuda.dll + cudart64_12.dll（本地
  build/ring_recon_cuda/bin，与 RingDisplay_c649a4e 包逐字节同源）、
  cufft64_12.dll、pa_recon_core.dll、libzmq-v141-mt-4_3_5.dll
  （以上 SHA 与 RingDisplay_c649a4e 包 MANIFEST 逐条一致）
- PAimageReceiverDiagnostics.ini（取自 RingDisplay_c649a4e 包 2026-09-29 上午
  实机运行后的最新设置；与开发机构建目录 INI 仅窗口几何不同）
- README.txt（实机三项验收操作口径、诊断旁证、依赖前提）
- MANIFEST_SHA256.txt（生成后 sha256sum -c 自检通过）
- 未随包（沿参考包口径）：ring_svc_selftest.exe、ring_udp_replay.exe、
  diagnostic-tools/

实机验收边界（四层证据分离）
----------------------------
软件/单测 PASS ≠ 实机 PASS。实机三项（时域/频域持续刷新、实时成像冻结不重置、
保存与卡片计数照常）由用户执行并在现场记录；执行代理不代为声明。
