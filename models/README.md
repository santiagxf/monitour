# Models

One ONNX file is required here before building:

| File | Purpose | Precision |
|---|---|---|
| `6drepnet.fp16.onnx` | Inference — runs on NPU (preferred), GPU, or CPU | FP16 |

The same FP16 model serves every backend. The Intel AI Boost NPU runs FP16
natively via the OpenVINO execution provider; DirectML and the CPU EP take
the same file as a fallback on hardware without an NPU.

INT8 isn't shipped today: it would require per-machine activation-range
calibration to stay accurate, and the FP16 model is already fast and
energy-efficient enough on the NPU. Add INT8 back only if a measurement says
the FP16 path doesn't meet the latency/power budget.

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
`rotation` (`[N,3,3]` rotation matrix). Convert to FP16 via
[Olive](https://github.com/microsoft/Olive). Olive resolves relative paths
against the current working directory, not the config file, so run from the
directory that holds `6drepnet.fp32.onnx`:

```bash
cd /path/to/6DRepNet/sixdrepnet
olive run --config /path/to/monitour/tools/olive/olive_fp16.json
mv ./model.onnx /path/to/monitour/models/6drepnet.fp16.onnx
```

## Verifying NPU dispatch

After running Monitour, check `%LOCALAPPDATA%\Monitour\monitour.log`. At
startup `HeadPoseModel` logs the resolved execution provider, e.g.:

```
HeadPoseModel: running on OpenVINOExecutionProvider on NPU (Intel)
```

`Task Manager → Performance → NPU` should show steady activity during use.
If the log shows `DmlExecutionProvider on GPU` instead, Windows ML didn't
register the OpenVINO NPU EP — usually because the Intel NPU driver is
missing or out of date, or the machine isn't Copilot+ class. Update the
driver via Windows Update or from intel.com.

The first run on a new machine pays a multi-second NPU compile cost while
the OpenVINO UMD compiles the graph; the result is cached under
`%LOCALAPPDATA%\Monitour\ov_cache\` and subsequent launches are
near-instant. Delete that directory if you replace the model file.
