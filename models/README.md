# Models

Place YOLOv8 ONNX models and optional class name files here.

Example:

```text
models/
  yolov8_defect.onnx
  classes.txt
```

The application also supports selecting a model from any local directory.

Current local test model:

```text
models/
  yolov8n.pt      # Source PyTorch checkpoint downloaded from Ultralytics.
  yolov8n.onnx    # Exported ONNX model loaded by the Qt/OpenCV application.
  classes.txt     # COCO class names used by yolov8n.
```

The `.pt` and `.onnx` files are ignored by Git because model files are large
runtime artifacts. Keep `classes.txt` aligned with the model you load.
