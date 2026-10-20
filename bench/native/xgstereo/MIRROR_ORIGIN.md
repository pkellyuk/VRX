# Mirror source provenance

`gen_mirror.py` and portions of `mirror_backend.h` are adapted from the local
vrdual checkout (`tools/gen_mirror.py`, `layer/vrdual_mirror.hpp`, and the inline
shader helper in `layer/vrdual_layer.cpp`), based on commit
`b9eba4b5b64815b87e34710e04671e0fb5a2890e`.
Upstream: https://github.com/adderek/vrdual . No top-level license was present
in the source checkout; resolve redistribution permission before publishing
these adapted files. This local prototype does not relicense upstream code.

VRX uses one device on one GPU, separate eye resources and command buffers,
Win32 memory shadows, and the shared D3D12 texture transport. It does not use
vrdual's Linux ICD loading, GPU selection, readback or frame-file transport.
