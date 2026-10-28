"""Open-H3-Regenerate-2K (NVlabs, Efficient-Large-Model/Open-H3-Regenerate-2K) -> one native ComfyUI MiniMax H3 LoRA.

The release is a diffusers-named merged LoRA (refiner + LightX2V FL2VA Turbo + LMS Sharp, alpha == rank, scale 1) plus
LMS Sharp's 50 AdaLN "curve branches" (B @ A on the 8-D timestep-curve coordinates). ComfyUI's pruned (curve-form) H3
checkpoints already feed those coordinates into blocks.N.adaln_proj.linear ([96768, 8]), so a curve branch is a plain
LoRA on that layer. Same layout rules as convert_wuli_4step.py: q/k/v fuse into qkv_proj (A stacked, B block-diagonal),
fc1's SwiGLU halves swap (diffusers [value; gate] -> ComfyUI [gate; value]).

usage: python convert_open_h3_regenerate.py <lora.safetensors> <sharp_curve_branches.safetensors> <base.safetensors> <out.safetensors>
"""
import json
import sys

import torch
from safetensors import safe_open
from safetensors.torch import save_file


def native_name(peft):  # (as convert_wuli_4step.py; copied: the setup runs Python isolated, -I, with no script folder on the path)
    name = peft.replace("token_refiner.refiner_blocks.", "token_refiner.blocks.").replace("transformer_blocks.", "blocks.")
    for a, b in ((".attn.to_out.0", ".attn.out_proj"), (".ff.net.0.proj", ".mlp.fc1"), (".ff.net.2", ".mlp.fc2")):
        if name.endswith(a):
            return name[: -len(a)] + b
    return name


def main(src, curves, base, dst):
    with safe_open(base, framework="pt", device="cpu") as f:
        keys = set(f.keys())
        pre = "diffusion_model." if any(k.startswith("diffusion_model.") for k in keys) else ""
        shapes = {k[len(pre):]: f.get_slice(k).get_shape() for k in keys if k.endswith(".weight")}

    out = {}
    with safe_open(src, framework="pt", device="cpu") as f:
        stems = sorted({k[len("transformer."):].rsplit(".lora_", 1)[0] for k in f.keys()})
        get = lambda s, ab: f.get_tensor(f"transformer.{s}.lora_{ab}.weight").float()
        attn = sorted({s.rsplit(".", 1)[0] for s in stems if s.rsplit(".", 1)[1] in ("to_q", "to_k", "to_v")})
        for a in attn:
            parts = [p for p in ("to_q", "to_k", "to_v") if f"{a}.{p}" in stems]
            if len(parts) != 3:
                raise SystemExit(f"{a}: q/k/v must all be there to fuse into qkv_proj")
            A = torch.cat([get(f"{a}.{p}", "A") for p in parts], 0)
            B = torch.block_diag(*[get(f"{a}.{p}", "B") for p in parts])
            n = native_name(a) + ".qkv_proj"
            check(shapes, n, B.shape[0], A.shape[1])
            out[f"diffusion_model.{n}.lora_A.weight"] = A.to(torch.bfloat16)
            out[f"diffusion_model.{n}.lora_B.weight"] = B.to(torch.bfloat16)
        for s in stems:
            if s.rsplit(".", 1)[1] in ("to_q", "to_k", "to_v"):
                continue
            A, B = get(s, "A"), get(s, "B")
            if s.endswith(".ff.net.0.proj"):
                h = B.shape[0] // 2
                B = torch.cat([B[h:], B[:h]], 0)
            n = native_name(s)
            check(shapes, n, B.shape[0], A.shape[1])
            out[f"diffusion_model.{n}.lora_A.weight"] = A.to(torch.bfloat16)
            out[f"diffusion_model.{n}.lora_B.weight"] = B.to(torch.bfloat16)

    with safe_open(curves, framework="pt", device="cpu") as f:
        roots = sorted({k[len("transformer."):-len(".curve_A.weight")] for k in f.keys() if k.endswith(".curve_A.weight")})
        if len(roots) != 50:
            raise SystemExit("expected the 50 Sharp curve AdaLN branches")
        for r in roots:
            A, B = f.get_tensor(f"transformer.{r}.curve_A.weight").float(), f.get_tensor(f"transformer.{r}.curve_B.weight").float()
            n = r.replace("transformer_blocks.", "blocks.")
            check(shapes, n, B.shape[0], A.shape[1])
            # adaln weights stay fp32 in ComfyUI (adaln_dtype); keep the branch fp32 too, it is small
            out[f"diffusion_model.{n}.lora_A.weight"] = A
            out[f"diffusion_model.{n}.lora_B.weight"] = B

    save_file(out, dst, metadata={
        "converted_layout": "comfyui_minimax_h3",
        "source": "Efficient-Large-Model/Open-H3-Regenerate-2K lora/ (merged refiner + LightX2V FL2VA Turbo + LMS Sharp, "
                  "and the Sharp curve AdaLN branches)",
        "conversion": "qkv block-diag fused; mlp.fc1 swiglu halves swapped; curve branches as adaln_proj.linear LoRAs; bf16 "
                      "(curve branches fp32)",
        "usage": json.dumps({"base": "minimax_h3_fl2va_pruned (curve form)", "strength": 1.0, "sampler": "euler", "cfg": 1,
                             "sigmas": [0.909375, 0.857142857, 0.666666667, 0.0],
                             "latent": "H3 VAE latent of the draft, MiniMax H3 latent upscaler to the target size"}),
    })
    print(f"wrote {dst}: {len(out)} tensors ({len(attn)} fused qkv, {len(roots)} curve branches)")


def check(shapes, name, rows, cols):
    want = shapes.get(name + ".weight")
    if want is None:
        raise SystemExit(f"{name}: not in the base model")
    if tuple(want) != (rows, cols):
        raise SystemExit(f"{name}: LoRA {rows}x{cols} does not match the base {tuple(want)}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
