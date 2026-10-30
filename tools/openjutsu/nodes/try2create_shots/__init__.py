# Try2Create's shot-list check for Openjutsu's swap (comfyui-h3-oneclick-swap), put here by Try2Create; safe to delete.
#
# The swap asks Qwen3-VL for one line per shot ("[Shot 2] At 00:01.917, the camera cuts to <the shot>") naming who is where, and that
# line is all that tells H3 which picture belongs in which shot after a hard cut. On a fast-cut clip (12 frames, 6 shots) the 8B model
# wrote the form back unchanged - "<the shot>" six times - and H3 drew Bowser in the first shot and the source people in every one after.
# This node sits between that answer and H3 Fill Shot List. The masks have the last word on who is in each shot: a line naming exactly
# the subjects the masks show there passes unchanged; any other (a placeholder, nobody, the wrong subject) is written from the masks -
# who is in the shot and where. What H3 gets goes to ComfyUI's log, with the vision model's answer when lines were rewritten.
import json
import re

import torch
import torch.nn.functional as F

FPS = 24
MIN_SHARE = 0.01  # of the frame, for a subject to count as in a shot


def find_shots(images, cut_threshold):
    """(start, end) frame ranges of the shots between hard cuts - the same as comfyui-h3-oneclick-swap's (MIT), so the shots match."""
    grey = torch.cat([F.interpolate(images[i:i + 64, ..., :3].mean(-1, keepdim=True).movedim(-1, 1).float(), size=(90, 160), mode="area")[:, 0]
                      for i in range(0, images.shape[0], 64)]) * 255
    diffs = [0.0] + (grey[1:] - grey[:-1]).abs().mean(dim=(1, 2)).tolist()
    starts = [0]
    for i in range(1, len(diffs)):
        around = sorted(diffs[max(1, i - 6):i] + diffs[i + 1:i + 7])
        median = around[len(around) // 2] if around else 0.0
        if diffs[i] > cut_threshold and diffs[i] > 2.5 * median and i - starts[-1] >= 6:
            starts.append(i)
    return list(zip(starts, starts[1:] + [images.shape[0]]))


def timestamp(seconds):
    return "%02d:%06.3f" % (seconds // 60, seconds % 60)


def where(xs, width):
    x = xs.float().mean().item() / max(1, width - 1)
    return "on the left" if x < 0.38 else "on the right" if x > 0.62 else "in the centre"


def people_in(role_map, a, b):
    """[(subject, place)] for the subjects the masks show in frames a..b, left to right."""
    m = role_map[a:b]
    if m.numel() == 0:
        return []
    found = []
    for k in sorted(set(torch.unique(m).tolist()) - {0}):
        on = m == k
        share = on.float().mean(dim=(1, 2))
        if share.max().item() < MIN_SHARE:
            continue
        f = int(share.argmax())
        xs = on[f].nonzero()[:, 1]
        found.append((xs.float().mean().item(), int(k), where(xs, m.shape[2])))
    return [(k, place) for _, k, place in sorted(found)]


def named(people):
    parts = ["<Subject %d> %s" % (k, place) for k, place in people]
    return parts[0] if len(parts) == 1 else ", ".join(parts[:-1]) + " and " + parts[-1]


class Try2CreateShotList:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "shots": ("STRING", {"forceInput": True, "tooltip": "The vision model's shot list."}),
                "images": ("IMAGE", {"tooltip": "This clip's frames, the ones H3 Clip Shots found the cuts in."}),
                "role_map": ("H3_ROLE_MAP", {"tooltip": "From H3 Role Mask."}),
                "cut_threshold": ("FLOAT", {"default": 40.0, "min": 1.0, "max": 128.0, "step": 0.5, "tooltip": "Same as H3 Clip Shots."}),
            },
        }

    RETURN_TYPES = ("STRING",)
    RETURN_NAMES = ("shots",)
    FUNCTION = "check"
    CATEGORY = "conditioning/minimax/context_loop"
    DESCRIPTION = "Keeps the shot lines that describe their shot; writes the rest from the masks (who is in each shot and where)."

    def check(self, shots, images, role_map, cut_threshold):
        bounds = find_shots(images, cut_threshold)
        given = {}
        for line in (shots or "").splitlines():
            m = re.match(r"\s*\**\s*\[Shot (\d+)\]\**\s*(.*)", line)
            if m:
                given.setdefault(int(m.group(1)), m.group(2).strip())
        # the role map is SAM's size, a frame per clip frame
        scale = role_map.shape[0] / max(1, images.shape[0])
        out, written = [], 0
        for k, (a, b) in enumerate(bounds, 1):
            people = people_in(role_map, int(a * scale), max(int(a * scale) + 1, int(b * scale)))
            text = given.get(k, "")
            lead = "" if k == 1 else "At %s, the camera cuts to " % timestamp(a / FPS)
            body = re.sub(r"^At \d+:\d+(?:\.\d+)?,\s*the camera cuts to\s*", "", text, flags=re.I)
            says = {int(n) for n in re.findall(r"<Subject (\d+)>", body)}
            if "<the shot>" in body.lower() or len(re.sub(r"<[^>]*>", "", body).strip(" .,")) < 3 or says != {s for s, _ in people}:
                written += 1
                body = (named(people) + "." if people else "a shot without the people being replaced; everyone in it stays as they are.")
                body = body[0].upper() + body[1:] if k == 1 else body
            out.append("[Shot %d] %s%s" % (k, lead, body))
        if written:
            print("[Try2Create] shot list: wrote %d of %d shot lines from the masks; the vision model's answer was:\n%s" % (written, len(bounds), (shots or "").strip()))
        print("[Try2Create] shot list for H3:\n" + "\n".join(out))
        return ("\n".join(out),)


class Try2CreateRoleFix:
    """Two people on screen together can't be the same subject. H3 Track Roles asks again and then swaps a duplicate to another subject by
    colour - but only to a subject it has seen somewhere: on a clip of a girl with a man behind her (his face behind fanned money the whole
    time) both came out Subject 1, Peach was never used, and Bowser was drawn over the two of them as one figure (2026-10-29). Here the
    smaller of two same-subject people on screen together gets a subject nobody has, if there is one."""

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"labels": ("STRING", {"forceInput": True, "tooltip": "From H3 Track Roles."}),
                             "segments": ("STRING", {"forceInput": True, "tooltip": "From H3 Track Crops."})}}

    RETURN_TYPES = ("STRING",)
    RETURN_NAMES = ("labels",)
    FUNCTION = "fix"
    CATEGORY = "conditioning/minimax/context_loop"
    DESCRIPTION = "Gives the smaller of two same-subject people on screen together a subject nobody has."

    def fix(self, labels, segments):
        data = json.loads(segments)
        segs = data.get("segments", [])
        out = json.loads(labels)
        if len(out) != len(segs):
            return (labels,)
        missing = [k for k in range(1, int(data.get("subjects", 0)) + 1) if k not in {l["role"] for l in out}]
        changed = []
        for i in range(len(out)):
            for j in segs[i].get("with", []):
                if not missing or j <= i or not out[i]["role"] or out[i]["role"] != out[j]["role"]:
                    continue
                small = i if segs[i].get("area", 0) < segs[j].get("area", 0) else j
                k = missing.pop(0)
                changed.append("track %d: Subject %d -> Subject %d" % (out[small]["track"], out[small]["role"], k))
                out[small]["role"] = k
        if changed:
            print("[Try2Create] role fix (two people on screen together had one subject): " + "; ".join(changed))
        return (json.dumps(out),)


NODE_CLASS_MAPPINGS = {"Try2CreateShotList": Try2CreateShotList, "Try2CreateRoleFix": Try2CreateRoleFix}
NODE_DISPLAY_NAME_MAPPINGS = {"Try2CreateShotList": "Try2Create Shot List Check (who is in each shot)",
                              "Try2CreateRoleFix": "Try2Create Role Fix (no two people on screen as one subject)"}
