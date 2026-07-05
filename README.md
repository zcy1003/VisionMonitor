# VisionMonitor

`VisionMonitor` 是一个面向求职作品集的工业视觉质检系统示例，技术栈为 Qt Widgets + C++17 + OpenCV。当前版本重点覆盖郑州 C++/Qt 岗位常见关键词：上位机界面、多线程采集、OpenCV 图像处理、YOLOv8 ONNX 推理、检测结果可视化、设备状态模拟和质检统计。

## 已实现功能

- Qt Widgets 质检工作站界面
- 摄像头实时采集线程
- 模拟设备数据线程：温度、压力、转速
- OpenCV 规则检测：未加载模型时也可以演示缺陷框选
- YOLOv8 ONNX 模型加载与推理
- OK/NG 判定、缺陷数量、推理耗时、FPS、良品率统计
- 离线图片检测
- 当前检测结果图保存
- 检测日志

## 模型使用方式

程序可以不加载模型运行，此时使用传统 OpenCV 阈值/轮廓规则检测亮斑、黑斑类表面异常。

如果要使用 YOLOv8：

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

## 项目结构

```text
VisionMonitor/
  CMakeLists.txt
  main.cpp
  mainwindow.h/.cpp        # 主界面、统计、模型加载、图片测试
  camerathread.h/.cpp      # 摄像头采集与检测线程
  serialthread.h/.cpp      # 模拟设备数据
  visiondetector.h/.cpp    # OpenCV 规则检测 + YOLOv8 ONNX 推理
```

## 简历描述建议

可以写成：

> 基于 Qt/C++、OpenCV 和 YOLOv8 ONNX 开发工业视觉质检系统，采用摄像头采集线程与 UI 主线程解耦，实现实时图像采集、目标/缺陷检测、OK/NG 判定、缺陷框可视化、推理耗时/FPS/良品率统计、离线图片检测和检测结果保存；未加载模型时支持传统 OpenCV 规则检测，便于现场调试和算法对比。

## 1-2 个月学习路线

### 第 1 周：把 Qt 工程基础补扎实

- CMake 构建 Qt6 项目
- Qt 信号槽、跨线程通信、对象生命周期
- QLabel 显示 OpenCV 图像
- QThread 采集摄像头并安全停止

阶段产出：能讲清楚本项目里 `CameraThread`、`SerialThread` 和主线程 UI 的关系。

### 第 2 周：OpenCV 工业视觉基础

- 灰度化、滤波、阈值、形态学
- 轮廓检测、外接矩形、面积过滤
- 图像坐标系、ROI、检测结果绘制
- 离线图片检测流程

阶段产出：能不用深度学习，完成黑点、亮点、划痕等简单缺陷检测 demo。

### 第 3 周：YOLOv8 推理部署

- 了解目标检测输入/输出格式
- 训练或获取一个缺陷检测模型
- 导出 ONNX
- 使用 OpenCV DNN 加载 ONNX
- 实现置信度过滤、NMS、类别名显示

阶段产出：能在本项目中加载 `.onnx` 模型，并解释从模型输出到检测框的解析过程。

### 第 4 周：上位机工程化

- 检测统计：OK/NG、良品率、检测耗时
- 日志记录
- 截图保存
- 参数配置
- 异常处理：摄像头打开失败、模型加载失败、空图像

阶段产出：项目从算法 demo 变成可演示的工业质检工作站。

### 第 5-6 周：贴近郑州岗位需求扩展

- 串口真实读取或 Modbus RTU/TCP
- SQLite 保存检测记录
- CSV/Excel 报表导出
- Linux 下编译运行
- 打包部署

阶段产出：简历关键词覆盖 Qt、C++、OpenCV、YOLOv8、上位机、串口/Modbus、SQLite、Linux。

### 第 7-8 周：面试准备

- 准备 3 分钟项目介绍
- 准备 5 个技术难点：跨线程图像传输、Mat/QImage 转换、YOLO 输出解析、NMS、检测耗时优化
- 准备项目截图和演示视频
- 准备代码仓库 README

## 下一步建议

优先继续补三项：

1. SQLite 检测记录表
2. 串口/Modbus 真实通信
3. Linux 编译说明

这三项和郑州 C++/Qt 上位机岗位的匹配度最高。
