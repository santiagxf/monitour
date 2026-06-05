# Models

Two ONNX variants of [6DRepNet](https://github.com/thohemp/6DRepNet) (MIT) must be placed here before building:

| File | Purpose | Quantization |
|---|---|---|
| `6drepnet.int8.onnx` | NPU path (Windows ML) | INT8 |
| `6drepnet.fp16.onnx` | DirectML fallback | FP16 |

## Export procedure

Upstream 6DRepNet has no `onnx_export` module, so use the export script in
[`tools/onnx_export.py`](../tools/onnx_export.py). Run it from inside the cloned
repo's `sixdrepnet/` directory (so the model's relative imports resolve):

```bash
git clone https://github.com/thohemp/6DRepNet.git
cd 6DRepNet
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt onnx onnxruntime olive-ai
# Download 6DRepNet_300W_LP_AFLW2000.pth from the repo's Google Drive link.

cd sixdrepnet
python /path/to/monitour/tools/onnx_export.py \
    --weights ../6DRepNet_300W_LP_AFLW2000.pth \
    --output 6drepnet.fp32.onnx --opset 17 --input-size 224
```

The exported graph has input `input` (`[N,3,224,224]` float32) and output
`rotation` (`[N,3,3]` rotation matrix). Then convert via
[Olive](https://github.com/microsoft/Olive) using the configs in `tools/olive/`:

```bash
cp /path/to/monitour/tools/olive/olive_*.json .
olive run --config olive_int8.json   # → 6drepnet.int8.onnx
olive run --config olive_fp16.json   # → 6drepnet.fp16.onnx
```

Copy the two `.onnx` outputs into this `models/` folder.

## Verifying NPU dispatch

After running Monitour, check the log: `DeviceFactory` logs the selected `LearningModelDeviceKind` at startup. Confirm `NPU` (or `DirectX` on non-Copilot+ hardware). In Task Manager → Performance → NPU you should see utilization spikes during use.
