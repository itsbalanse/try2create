"""Convert Wuli-art's MiniMax-H3-Turbo 4-step adapter into a LoRA that native ComfyUI loads.

Wuli ships Wuli-MiniMax-H3-Turbo-4steps-V1.0-bf16.safetensors for its own diffusers node pack: a PEFT LoRA in
diffusers names plus four output heads (one per step). Native ComfyUI (comfy/ldm/minimax) reads output heads as a
"PDD head bank" ([N*out, in], row block 0 a full head, later blocks offsets from it), stored in a normal LoRA file
as a pad-and-add reshape_weight patch (ComfyUI PR #15908). This writes that layout, so the stock LoraLoaderModelOnly
works and nothing from Wuli's pack (diffusers==0.40.0) goes into ComfyUI.

Use it on the official FL2VA transformer (Comfy-Org minimax_h3_fl2va_*: pruned or not, any quant), at strength 1.0,
4 steps, `simple` scheduler, euler, CFG 1, ModelSamplingMiniMaxH3 shift_video 6 / shift_audio 3.

    python convert_wuli_4step.py <wuli.safetensors> <fl2va base .safetensors | heads .safetensors> <out.safetensors>

Only the base's final_layer heads are read (the bank is stored relative to them).
"""
import json
import sys

import torch
from safetensors import safe_open
from safetensors.torch import save_file

STEPS = 4


def shifted(sigma, shift):
    return shift * sigma / (1 + (shift - 1) * sigma)


def native_name(peft):
    # transformer_blocks.N.attn.to_q -> blocks.N.attn.<q>; token_refiner.refiner_blocks.N -> token_refiner.blocks.N
    name = peft.replace("token_refiner.refiner_blocks.", "token_refiner.blocks.").replace("transformer_blocks.", "blocks.")
    for a, b in ((".attn.to_out.0", ".attn.out_proj"), (".ff.net.0.proj", ".mlp.fc1"), (".ff.net.2", ".mlp.fc2")):
        if name.endswith(a):
            return name[: -len(a)] + b
    return name


def convert(src, base, dst):
    with safe_open(src, framework="pt", device="cpu") as f:
        meta = f.metadata() or {}
        cfg = json.loads(meta["pdd_config"])
        lcfg = json.loads(meta["lora_adapter_metadata"])
        t = {k: f.get_tensor(k) for k in f.keys()}
    if cfg.get("format") != "wuli_minimax_h3_turbo_v1" or int(cfg.get("nfe", 0)) != STEPS:
        raise SystemExit("not a Wuli MiniMax-H3 Turbo V1 4-step file")
    grid, scale = int(cfg["grid_size"]), float(lcfg["lora_alpha"]) / float(lcfg["r"])
    block = grid // STEPS

    with safe_open(base, framework="pt", device="cpu") as f:
        keys = set(f.keys())
        pre = "diffusion_model." if "diffusion_model.final_layer.video_out.weight" in keys else ""
        bh = {k: f.get_tensor(pre + "final_layer." + k).float() for k in ("video_out.weight", "video_out.bias", "audio_out.weight", "audio_out.bias")}

    out = {}
    # backbone LoRA: q/k/v fuse into qkv_proj (A stacked, B block-diagonal); fc1's SwiGLU halves swap
    # (diffusers [value; gate] -> ComfyUI [gate; value]); alpha/r folded into B
    lora = {k[len("lora.generator."):]: v for k, v in t.items() if k.startswith("lora.generator.")}
    stems = sorted({k.rsplit(".lora_", 1)[0] for k in lora})
    attn = sorted({s.rsplit(".", 1)[0] for s in stems if s.rsplit(".", 1)[1] in ("to_q", "to_k", "to_v")})
    for a in attn:
        A = [lora[f"{a}.{p}.lora_A.weight"].float() for p in ("to_q", "to_k", "to_v")]
        Bs = [lora[f"{a}.{p}.lora_B.weight"].float() * scale for p in ("to_q", "to_k", "to_v")]
        n = native_name(a) + ".qkv_proj"
        out[f"diffusion_model.{n}.lora_A.weight"] = torch.cat(A, 0).to(torch.bfloat16)
        out[f"diffusion_model.{n}.lora_B.weight"] = torch.block_diag(*Bs).to(torch.bfloat16)
    for s in stems:
        if s.rsplit(".", 1)[1] in ("to_q", "to_k", "to_v"):
            continue
        A, B = lora[f"{s}.lora_A.weight"].float(), lora[f"{s}.lora_B.weight"].float() * scale
        if s.endswith(".ff.net.0.proj"):
            h = B.shape[0] // 2
            B = torch.cat([B[h:], B[:h]], 0)
        n = native_name(s)
        out[f"diffusion_model.{n}.lora_A.weight"] = A.to(torch.bfloat16)
        out[f"diffusion_model.{n}.lora_B.weight"] = B.to(torch.bfloat16)

    # output heads: Wuli's head i is the update over block i of the 256-grid; divided by that block's shifted-sigma
    # span it is a plain velocity head. Bank on a 4-cell grid: row 0 = head 0, row i = head i - head 0, stored as a
    # diff from the base head (pad-and-add: rows past the first start at zero)
    base_sigma = torch.linspace(1, 0, grid + 1, dtype=torch.float64)
    for mod, head, shift_key in (("video", "video_out", "video_flow_shift"), ("audio", "audio_out", "audio_flow_shift")):
        sig = shifted(base_sigma, float(cfg[shift_key]))
        W = [t[f"pdd.{mod}.{i}.weight"].double() / float(sig[i * block] - sig[(i + 1) * block]) for i in range(STEPS)]
        b = [t[f"pdd.{mod}.{i}.bias"].double() / float(sig[i * block] - sig[(i + 1) * block]) for i in range(STEPS)]
        bank_w = torch.cat([W[0] - bh[head + ".weight"].double()] + [W[i] - W[0] for i in range(1, STEPS)], 0).float()
        bank_b = torch.cat([b[0] - bh[head + ".bias"].double()] + [b[i] - b[0] for i in range(1, STEPS)], 0).float()
        rows = bank_w.shape[0]
        k = f"diffusion_model.final_layer.{head}"
        out[k + ".lora_up.weight"] = torch.eye(rows, dtype=torch.bfloat16)
        out[k + ".lora_down.weight"] = bank_w
        out[k + ".reshape_weight"] = torch.tensor(list(bank_w.shape), dtype=torch.int64)
        out[k + ".bias.lora_up.weight"] = bank_b.reshape(rows, 1)
        out[k + ".bias.lora_down.weight"] = torch.ones(1, 1)
        out[k + ".bias.reshape_weight"] = torch.tensor([rows], dtype=torch.int64)

    save_file(out, dst, metadata={
        "converted_layout": "comfyui_minimax_h3",
        "source": "Wuli-art/MiniMax-H3-Turbo turbo-4step V1.0",
        "conversion": "qkv block-diag fused; mlp.fc1 swiglu halves swapped; alpha/rank folded into lora_B; "
                      "4 step heads as a base-relative pad-and-add reshape_weight PDD bank",
        "pdd_num_steps": str(STEPS), "pdd_config": json.dumps(cfg),
        "usage": "FL2VA base, strength 1.0, 4 steps, simple, euler, cfg 1, shift_video %s, shift_audio %s"
                 % (cfg["video_flow_shift"], cfg["audio_flow_shift"]),
    })
    print(f"wrote {dst}: {len(out)} tensors; use shift_video {cfg['video_flow_shift']} / shift_audio {cfg['audio_flow_shift']}, 4 steps, simple")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    convert(*sys.argv[1:])
