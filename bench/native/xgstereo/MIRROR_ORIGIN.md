# Mirror source provenance

`gen_mirror.py` and portions of `mirror_backend.h` are adapted from vrdual
(`tools/gen_mirror.py`, `layer/vrdual_mirror.hpp`, and the inline shader helper
in `layer/vrdual_layer.cpp`), based on commit
`b9eba4b5b64815b87e34710e04671e0fb5a2890e`.
Upstream: https://github.com/adderek/vrdual

## Licence

vrdual had no licence at that commit. Its author, Maciej Wakuła (GitHub:
adderek), licensed the project under the Apache License 2.0, with a NOTICE file
naming the origin, on 2026-10-20 in commit
`c372a91f0febb2e915ac2ae9232b0a238894a39a` ("License: Apache-2.0 with a NOTICE
naming the origin"). The adapted files are used under those terms:

- The Apache-2.0 licence text and vrdual's NOTICE, verbatim, are in
  `release/licenses/vrdual-LICENSE.txt` and `release/licenses/vrdual-NOTICE.txt`,
  and every VRX release ships them in its `licenses` folder (Apache-2.0 section
  4(a) and 4(d)). `THIRD-PARTY-NOTICES.txt` names vrdual and points to both.
- VRX's derived files are modified (Apache-2.0 section 4(b)): they were rewritten
  for VRX's single-device mirror (see below), and the generator emits VRX's own
  dispatch tables. The first lines of `gen_mirror.py` and `mirror_backend.h` say
  they are adapted from vrdual and point here; this file records the changes.
- They are distributed as part of VRX, a GPL-3.0 work. Apache-2.0 is compatible
  with GPL-3.0, so the combined work is distributed under GPL-3.0, while the
  adapted portions keep vrdual's copyright and Apache-2.0 notice.

## What VRX does differently

VRX uses one device on one GPU, separate eye resources and command buffers,
Win32 memory shadows, and the shared D3D12 texture transport. It does not use
vrdual's Linux ICD loading, GPU selection, readback or frame-file transport.

VRX-authored changes to the adapted files include: per-queue layer timelines
instead of acquire-time submissions and `vkDeviceWaitIdle`; GPU zeroing of eye
offsets and release of right-eye resources after a stop (including generated
destroy wrappers); draw-time push-pointer translation with an interval map
(`mirror_addresses.h`, independently authored); last-pre-rasterisation-stage
planning; sRGB byte-copy export; and the generated device-extension allowlist.
