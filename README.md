# VisionMonitor

`VisionMonitor` 是一个面向求职展示和学习的 Qt + C++ + OpenCV 工业视觉质检项目。项目目标是把常见上位机能力组合到一个可演示的桌面端程序中：实时图像采集、OpenCV/ONNX 检测、设备通信、SQLite 追溯、报警、曲线、报表和自动检测/故障报告。

## 当前状态

已完成主要业务功能，正在补齐 Windows/Linux 双平台构建和部署细节。当前代码已将 OpenCV 路径、摄像头后端和运行时部署按平台拆分，Windows 本机可直接构建运行；Linux 侧需要准备 Qt6、OpenCV 和摄像头权限后编译验证。

## 功能清单

- Qt Widgets 桌面端质检工作站界面
- 摄像头实时采集线程，UI 主线程和采集线程解耦
- Windows 摄像头后端优先 DirectShow，Linux 优先 V4L2，并用非空帧验证后端可用
- OpenCV 规则检测，未加载模型时也可演示缺陷检测流程
- YOLOv8 ONNX 模型加载、推理、置信度过滤、NMS 和检测框绘制
- 离线图片检测、实时摄像头检测、当前结果截图保存
- 串口、TCP Client、Modbus TCP、Modbus RTU 和模拟数据源
- 温度、压力、转速状态显示和实时曲线窗口
- 阈值报警、报警确认、报警历史查询
- SQLite 检测记录、报警记录、NG 图片归档和历史追溯
- CSV 报表导出
- 基于本地统计规则的自动检测/故障报告生成
- `CameraProbe` 摄像头后端诊断工具

## 技术栈

- C++17
- Qt 6 Widgets / Network / SerialPort / Sql
- OpenCV / OpenCV DNN
- SQLite
- CMake

## 目录结构

```text
VisionMonitor/
  CMakeLists.txt
  main.cpp
  mainwindow.h/.cpp        # 主界面、业务流程、报警、曲线、报表、AI报告
  camerathread.h/.cpp      # 摄像头采集线程和实时检测
  serialthread.h/.cpp      # 串口/TCP/Modbus/模拟数据通信线程
  visiondetector.h/.cpp    # OpenCV 规则检测和 YOLOv8 ONNX 推理
  databasemanager.h/.cpp   # SQLite 检测记录、报警记录和图片归档
  models/                  # 类别文件和模型说明
  tools/
    camera_probe.cpp       # 摄像头后端诊断工具
    check_comment_policy.ps1
    install_git_hooks.ps1
    tcp_sensor_server.py
    modbus_tcp_server.py
```

## Windows 构建

环境建议：

- Windows 10/11
- Visual Studio 2022 或更新版本的 MSVC x64 工具链
- Qt 6.5+
- OpenCV 4.x
- CMake 3.19+

如果 OpenCV 安装在 `D:/OpenCV/opencv/build`，CMake 会在 Windows 上自动作为兜底路径使用。其他路径建议显式传入：

```powershell
cmake -S . -B build/windows-msvc `
  -G "NMake Makefiles" `
  -DQt6_DIR="D:/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6" `
  -DVISIONMONITOR_OPENCV_DIR="D:/OpenCV/opencv/build"

cmake --build build/windows-msvc --target VisionMonitor
```

如果使用 Qt Creator，直接打开 `CMakeLists.txt`，确认 Kit 指向 MSVC x64、Qt6 和 OpenCV 后构建即可。

## Linux 构建

Linux 侧推荐先安装系统依赖：

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  qt6-base-dev \
  qt6-base-dev-tools \
  qt6-tools-dev \
  qt6-tools-dev-tools \
  libqt6serialport6-dev \
  libqt6sql6-sqlite \
  libopencv-dev \
  v4l-utils \
  libgl1-mesa-dev \
  libglu1-mesa-dev \
  mesa-common-dev \
  libxkbcommon-dev \
  libxkbcommon-x11-dev \
  libvulkan-dev
```

Ubuntu 22.04 默认 Qt 版本通常是 6.2.4。项目 CMake 已兼容 Qt 6.2，
但系统必须安装完整的 Qt Widgets/Gui 开发依赖。若 CMake 提示
`WrapOpenGL`、`Qt6Gui`、`Qt6Widgets`、`XKB` 或 `Vulkan_INCLUDE_DIR`
缺失，通常就是上面这些 Mesa/OpenGL、XKB 或 Vulkan 开发包没有装完整。

然后构建：

```bash
rm -rf build/linux-release
cmake -S . -B build/linux-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-release --target VisionMonitor CameraProbe
```

如果 OpenCV 不是系统包安装，可以传入 OpenCV 的 CMake 包路径：

```bash
cmake -S . -B build/linux-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVISIONMONITOR_OPENCV_DIR=/opt/opencv/lib/cmake/opencv4
```

Linux 使用摄像头前需要确认当前用户有 `/dev/video*` 访问权限。常见做法是把用户加入 `video` 组，重新登录后生效：

```bash
sudo usermod -aG video $USER
```

## 摄像头诊断

主程序不会只依赖 `isOpened()` 判断摄像头可用，而是要求实际读到非空帧。可以先构建并运行诊断工具：

```bash
cmake --build build/linux-release --target CameraProbe
./build/linux-release/CameraProbe
```

Windows 下 `CameraProbe` 会依次测试 DirectShow、Media Foundation 和默认后端；Linux 下会测试 V4L2 和默认后端。输出中同时出现 `open=yes` 和 `frame=yes` 才说明该后端可用于主程序。

## 模型使用

程序可以不加载模型运行，此时使用传统 OpenCV 规则检测亮斑、暗斑等简单表面异常，方便没有训练模型时演示完整流程。

使用 YOLOv8 ONNX：

1. 准备 `.onnx` 模型，例如 `yolov8n.onnx` 或自己训练的工业缺陷模型。
2. 点击界面中的 `加载 YOLOv8 ONNX`。
3. 如果模型目录下存在 `classes.txt`，程序会按文件中的类别名显示检测结果。

`classes.txt` 示例：

```text
scratch
stain
crack
missing
```

当前 ONNX 推理走 OpenCV DNN CPU 路径。实时摄像头检测时，YOLO 会按间隔帧运行，跳过帧复用上一帧检测框，用来降低 CPU 推理导致的画面卡顿。

Ubuntu 22.04 默认仓库中的 OpenCV 通常是 4.5.4，这个版本的 DNN 模块对新版本 Ultralytics 导出的 YOLOv8 ONNX 兼容性有限。如果加载模型时提示 `shape_utils.hpp` 或 `OpenCV DNN 无法执行该 ONNX`，优先重新导出静态模型：

```bash
cd ~/VisionMonitor
yolo export model=yolov8n.pt format=onnx imgsz=640 opset=12 dynamic=False simplify=False nms=False
mv yolov8n.onnx models/yolov8n-opencv454.onnx
```

如果静态导出后仍然失败，说明当前系统的 C++ OpenCV 运行库太旧，需要升级项目链接的 OpenCV，而不是安装 `opencv-python`。`opencv-python` 只影响 Python 环境，不会改变本 Qt/C++ 程序链接到的 `libopencv-dev`。

## 设备通信

界面右侧可以选择通信方式：

- 模拟数据：不依赖外部设备，适合演示报警、曲线和报表。
- 真实串口：读取换行分隔的文本数据。
- TCP 客户端：连接外部传感器服务或 `tools/tcp_sensor_server.py`。
- Modbus TCP：按配置的站号和起始寄存器读取设备数据。
- Modbus RTU：通过串口读取 Modbus 寄存器。

当前约定的传感器数据为温度、压力、转速三项。真实项目落地时，应根据设备手册继续配置寄存器地址、倍率、字节序和异常重连策略。

## 数据与报表

程序启动后会初始化 SQLite 数据库，保存：

- 检测时间、来源、OK/NG、缺陷数量、标签、最高置信度、推理耗时和模型名
- 报警时间、等级、指标、当前值、阈值、确认状态
- 普通截图和 NG 归档图片路径

历史记录窗口用于追溯检测结果，报警窗口用于确认和查看报警。CSV 导出会把检测历史和报警历史写入一个 UTF-8 报表文件。

## 自动检测/故障报告

当前报告生成是本地规则版：程序读取 SQLite 中的检测记录和报警记录，统计 OK/NG、缺陷分布、报警分布，并生成原因推断和处理建议。后续如果要做成真正的大模型报告，可以把 `MainWindow::buildAiReport()` 替换为大模型 API 调用，同时保留本地统计结果作为输入上下文。

## 简历描述建议

可以写成：

> 基于 Qt/C++、OpenCV 和 YOLOv8 ONNX 开发工业视觉质检上位机，采用采集线程、通信线程与 UI 主线程解耦，实现实时图像采集、OpenCV/ONNX 缺陷检测、OK/NG 判定、串口/TCP/Modbus 设备通信、SQLite 质量追溯、报警管理、实时曲线、CSV 报表和自动检测/故障报告生成；针对 Windows DirectShow/MSMF 和 Linux V4L2 摄像头后端差异做了跨平台适配。

## 后续优化方向

- 在真实 Linux 环境完成编译、摄像头和串口验证，并补充截图。
- 将通信寄存器、倍率、阈值、模型路径等参数做成配置文件或设置界面。
- 增加 ONNX 模型输入/输出格式说明，方便替换不同 YOLO 导出版本。
- 接入大模型 API，把本地规则报告升级为更自然的故障分析报告。
- 补充演示视频、项目架构图和面试讲解材料。

## 开发规范

本项目要求新增或修改 C++、CMake、脚本代码时同步补充必要注释，尤其是平台兼容、线程安全、资源释放、图像算法、模型推理、NMS 和坐标换算等位置。提交前建议运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_comment_policy.ps1
```
