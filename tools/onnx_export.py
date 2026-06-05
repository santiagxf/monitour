#!/usr/bin/env python3
"""Export a trained 6DRepNet checkpoint to ONNX.

Upstream 6DRepNet (https://github.com/thohemp/6DRepNet) has no ``onnx_export``
module, so this script reproduces what the README expects. It builds the
inference (``deploy=True``) RepVGG-B1g2 model, loads a ``.pth`` snapshot, and
traces it to ONNX.

The model's ``forward`` returns a batched 3x3 rotation matrix
(``compute_rotation_matrix_from_ortho6d``), so the exported graph has output
shape ``[N, 3, 3]``. The C++ side decodes that to yaw/pitch/roll.

Run it from *inside* the cloned repo's ``sixdrepnet/`` directory so the model's
``import utils`` / ``from backbone.repvgg import ...`` resolve:

    git clone https://github.com/thohemp/6DRepNet.git
    cd 6DRepNet/sixdrepnet
    python /path/to/monitour/tools/onnx_export.py \
        --weights ../6DRepNet_300W_LP_AFLW2000.pth \
        --output 6drepnet.fp32.onnx --opset 17 --input-size 224
"""
import argparse
import sys

import torch

try:
    from model import SixDRepNet  # when run from inside sixdrepnet/
except ImportError:  # fall back to the pip package layout
    from sixdrepnet.model import SixDRepNet


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Export 6DRepNet to ONNX.")
    p.add_argument("--weights", required=True, help="Path to the .pth snapshot.")
    p.add_argument("--output", required=True, help="Destination .onnx file.")
    p.add_argument("--opset", type=int, default=17, help="ONNX opset version.")
    p.add_argument("--input-size", type=int, default=224, help="Square input size.")
    p.add_argument(
        "--backbone", default="RepVGG-B1g2", help="RepVGG backbone variant."
    )
    return p.parse_args()


def load_model(weights: str, backbone: str) -> torch.nn.Module:
    model = SixDRepNet(
        backbone_name=backbone,
        backbone_file="",
        deploy=True,
        pretrained=False,
    )
    checkpoint = torch.load(weights, map_location="cpu")
    if isinstance(checkpoint, dict) and "model_state_dict" in checkpoint:
        checkpoint = checkpoint["model_state_dict"]
    if isinstance(checkpoint, dict) and "state_dict" in checkpoint:
        checkpoint = checkpoint["state_dict"]
    state = {k.replace("module.", ""): v for k, v in checkpoint.items()}
    model.load_state_dict(state)
    model.eval()
    return model


def main() -> int:
    args = parse_args()
    model = load_model(args.weights, args.backbone)

    dummy = torch.randn(1, 3, args.input_size, args.input_size, dtype=torch.float32)
    export_kwargs = dict(
        export_params=True,
        opset_version=args.opset,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["rotation"],
        dynamic_axes={"input": {0: "batch"}, "rotation": {0: "batch"}},
    )
    # Force the legacy TorchScript exporter. The dynamo exporter (default in
    # recent PyTorch) emits a newer opset and then down-converts to the target
    # via onnxscript's version converter, which fails on this graph with
    # "No initializer or constant input to node found". The TorchScript path
    # targets the requested opset directly and avoids that converter.
    try:
        torch.onnx.export(model, dummy, args.output, dynamo=False, **export_kwargs)
    except TypeError:
        # Older PyTorch without a ``dynamo`` parameter.
        torch.onnx.export(model, dummy, args.output, **export_kwargs)
    print(f"Wrote {args.output} (opset {args.opset}, input {args.input_size}x{args.input_size}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
