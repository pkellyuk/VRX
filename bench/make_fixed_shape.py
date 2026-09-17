"""Export a fixed-shape variant of the dynamic-shape DA-V2-Small ONNX model.

Hypothesis under test: reference-app's models are fixed-shape exports; ours is dynamic
([-1,3,-1,-1]). Dynamic shapes can prevent the DML EP from fusing/optimizing
aggressively and can leave nodes unassigned to the EP.

Supports non-square geometry so we can measure reference-app's 686x392 directly.

usage: python make_fixed_shape.py <src.onnx> <dst.onnx> <height> [width]
"""
import sys

import onnx

src, dst = sys.argv[1], sys.argv[2]
h = int(sys.argv[3])
w = int(sys.argv[4]) if len(sys.argv) > 4 else h

# DA-V2's patch size is 14, and the model's output height/width are
# 14*floor(in/14). Keep the declared output consistent with that.
out_h = 14 * (h // 14)
out_w = 14 * (w // 14)

m = onnx.load(src)
print(f"loaded {src}")

for inp in m.graph.input:
    dims = inp.type.tensor_type.shape.dim
    if len(dims) != 4:
        print(f"  skip input {inp.name} (rank {len(dims)})")
        continue
    print(f"  input {inp.name}: {[d.dim_param or d.dim_value for d in dims]} -> [1,3,{h},{w}]")
    for d, v in zip(dims, [1, 3, h, w]):
        d.dim_param = ""
        d.dim_value = v

for out in m.graph.output:
    dims = out.type.tensor_type.shape.dim
    if len(dims) != 3:
        print(f"  skip output {out.name} (rank {len(dims)})")
        continue
    print(f"  output {out.name}: {[d.dim_param or d.dim_value for d in dims]} -> [1,{out_h},{out_w}]")
    for d, v in zip(dims, [1, out_h, out_w]):
        d.dim_param = ""
        d.dim_value = v

try:
    m = onnx.shape_inference.infer_shapes(m, strict_mode=False)
    print("shape inference: ok")
except Exception as exc:
    print(f"shape inference warning: {exc}")

onnx.save(m, dst)
print(f"saved {dst}")

chk = onnx.load(dst)
for inp in chk.graph.input:
    dims = inp.type.tensor_type.shape.dim
    print("  verify input :", inp.name, [d.dim_param or d.dim_value for d in dims])
for out in chk.graph.output:
    dims = out.type.tensor_type.shape.dim
    print("  verify output:", out.name, [d.dim_param or d.dim_value for d in dims])
