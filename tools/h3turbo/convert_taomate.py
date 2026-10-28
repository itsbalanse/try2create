"""Convert TaoLiveAIGC's TaoMate-H3-FL2VA adapter into a LoRA that native ComfyUI loads (an experiment).

TaoMate ships a rank-128 fp32 LoRA (`<target>.lora_a` [r, in], `<target>.lora_b` [out, r], alpha 128) over the 52 x 4 H3
projections (token_refiner.blocks.0-1 and blocks.0-49: attn.qkv_proj, attn.out_proj, mlp.fc1, mlp.fc2), in MiniMax's own
module names, which are ComfyUI's native H3 names too. fc1 holds [gate; up] like ComfyUI, so nothing is reordered (Wuli's
diffusers file needed a swap). There are no extra output heads. Every target is checked against the base's weight shape.

It was trained for TaoMate's streaming runtime (blockwise, causal, with a clean AV KV cache, 3 steps per block); ComfyUI runs
it on the whole clip at once, which is outside what it was trained for. Their student samples 3 steps: the 50-point schedule
kept at [0, 16, 33, 49], shift 12 video / 3 audio, no guidance, which ComfyUI's `simple` scheduler at 3 steps matches closely.

    python convert_taomate.py <adapter_model.safetensors> <fl2va base .safetensors> <out.safetensors>
"""
import json
import struct
import sys

import torch
from safetensors import safe_open
from safetensors.torch import save_file

RANK, ALPHA = 128, 128.0


def targets():
    blocks = [f"token_refiner.blocks.{i}" for i in range(2)] + [f"blocks.{i}" for i in range(50)]
    return [f"{b}.{s}" for b in blocks for s in ("attn.qkv_proj", "attn.out_proj", "mlp.fc1", "mlp.fc2")]


def base_shapes(path):
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(n))
    return {k: v["shape"] for k, v in header.items() if k != "__metadata__"}


def convert(src, base, dst):
    shapes = base_shapes(base)
    pre = "diffusion_model." if any(k.startswith("diffusion_model.") for k in shapes) else ""
    out, scale = {}, ALPHA / RANK
    with safe_open(src, framework="pt", device="cpu") as f:
        keys = set(f.keys())
        want = {f"{t}.{leaf}" for t in targets() for leaf in ("lora_a", "lora_b")}
        if keys != want:
            raise SystemExit(f"not the TaoMate-H3 adapter layout: missing {len(want - keys)}, unexpected {len(keys - want)}")
        for t in targets():
            a, b = f.get_tensor(f"{t}.lora_a").float(), f.get_tensor(f"{t}.lora_b").float() * scale
            w = shapes.get(f"{pre}{t}.weight")
            if w is None:
                raise SystemExit(f"the base has no {pre}{t}.weight")
            if a.shape[0] != RANK or b.shape[1] != RANK or [b.shape[0], a.shape[1]] != list(w[:2]):
                raise SystemExit(f"{t}: LoRA {tuple(b.shape)} x {tuple(a.shape)} does not fit the base weight {w}")
            out[f"diffusion_model.{t}.lora_A.weight"] = a.to(torch.bfloat16)
            out[f"diffusion_model.{t}.lora_B.weight"] = b.to(torch.bfloat16)
    save_file(out, dst, metadata={
        "converted_layout": "comfyui_minimax_h3",
        "source": "TaoLiveAIGC/TaoMate-H3-FL2VA step-300 online generator LoRA (rank 128, alpha 128)",
        "conversion": "renamed to diffusion_model.<target>.lora_A/B; alpha/rank folded into lora_B; bf16",
        "usage": "FL2VA base, strength 1.0, 3 steps, simple, euler, cfg 1, shift_video 12, shift_audio 3 (experimental: trained for streaming)",
    })
    print(f"wrote {dst}: {len(out)} tensors, {len(targets())} targets fit the base")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    convert(*sys.argv[1:])
