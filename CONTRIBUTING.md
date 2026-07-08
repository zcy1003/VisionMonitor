# VisionMonitor 代码贡献规范

## 注释要求

后续每次新增或修改代码时，都要同步补充必要注释。注释的目标是解释业务意图、平台差异、异常处理原因和关键算法步骤，而不是逐行复述代码。

推荐优先添加注释的位置：

- 跨线程通信、资源生命周期、摄像头/串口等外设打开和释放逻辑。
- Windows、Linux、OpenCV、Qt 等平台或库行为差异。
- 图像处理、YOLO 推理、NMS、坐标换算等不容易一眼看懂的算法步骤。
- 为兼容真实工业现场而增加的兜底、重试、降级和错误提示。

不建议添加的注释：

- 对简单赋值、普通控件创建、直接函数调用进行重复解释。
- 与当前实现不一致的历史说明。
- 大段空泛说明，导致真正关键的技术原因被淹没。

## 摄像头采集说明

当前 Windows 笔记本上 OpenCV 的 MSMF(Media Foundation) 链路存在“设备能打开但读不到帧”的异常，而 DirectShow 可以正常采集。因此项目中的摄像头打开逻辑必须验证实际读帧结果，不能只依赖 `isOpened()`。

摄像头后端优先级：

1. `cv::CAP_DSHOW`：优先使用，当前机器验证可返回有效帧。
2. `cv::CAP_MSMF`：保留为兼容其他 Windows 环境的备选方案。
3. `cv::CAP_ANY`：最后使用 OpenCV 默认策略兜底。

如需重新排查摄像头问题，可以构建并运行 `CameraProbe`，观察每个后端是否同时满足 `open=yes` 和 `frame=yes`。

## 注释检查脚本

提交前可以运行下面的命令检查当前工作区：

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_comment_policy.ps1
```

如果只想检查已经暂存的改动，可以运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_comment_policy.ps1 -Mode staged
```

该脚本只做基础检查：有新增实现代码的 C++、CMake 或脚本文件，必须同时新增或更新注释。它不能替代人工判断，注释是否准确仍以本文件和 `AGENTS.md` 的规则为准。

## Git hook

可以运行下面的命令启用本仓库的提交前检查：

```powershell
powershell -ExecutionPolicy Bypass -File tools/install_git_hooks.ps1
```

启用后，Git 会在每次提交前自动执行 `.githooks/pre-commit`，并检查暂存区是否满足注释规则。该配置只写入当前仓库的本地 Git 配置，不会影响其他项目。
