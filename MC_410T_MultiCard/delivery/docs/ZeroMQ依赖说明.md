# ZeroMQ 依赖说明

## 当前角色

canonical main 中 ZeroMQ 用于：

1. `ImagingController <-> ImagingSvc` 的成像 IPC；
2. 可选 `FramePublisher` 扩展。

正式构建不依赖历史文档中的外部 `D:\Program Files\zeromq-4.3.5` 绝对路径；
当前依赖入口是：

```text
MC_410T_MultiCard/delivery/third_party/zeromq/
  include/
  lib/
  bin/
```

正式 dependency provenance / SHA256 以根目录 `BUILD_STANDARD.md` 为准。

## MinGW canonical build

当前 MinGW production / ImagingSvc 路径使用：

```text
include/zmq.h
lib/libzmq.dll.a
bin/libzmq-v141-mt-4_3_5.dll
```

`CMakeLists.txt` 中 Imaging IPC 无条件链接该 import library，并在 post-build 把
`libzmq-v141-mt-4_3_5.dll` 部署到目标目录。

可选 `USE_ZEROMQ` FramePublisher：

- MinGW：同样使用 `libzmq.dll.a` + `libzmq-v141-mt-4_3_5.dll`；
- MSVC：代码仍保留 v143 import/runtime 名称分支，但这不是当前正式 MinGW delivery 路径。

## Rebuild boundary

通常不应在普通功能任务中顺手重编 ZeroMQ。若确实需要重建：

1. 固定 ZeroMQ 4.3.5 source；
2. 记录 MSVC/toolchain 版本；
3. 生成与 MinGW 可链接的 `.dll.a` import library；
4. 确认最终 runtime 文件名与当前 CMake/BUILD_STANDARD 一致；
5. 重新计算 SHA256；
6. 在正式 Windows build + ImagingSvc/ring selftest 中验证。

不要从随机下载站替换 runtime DLL，也不要只因为“DLL 能加载”就更新正式依赖。

## 历史说明

仓库曾保存一份 2026-04 的外部 ZeroMQ 编译教程，包含 v143 runtime、
外部安装目录和早期 FramePublisher 示例。该文档已经不符合当前 canonical dependency layout，
因此不再保留为当前说明；需要时可从 Git history 恢复。
