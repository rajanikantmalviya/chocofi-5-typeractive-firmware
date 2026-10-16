"""Validate device-local IDs and regenerate only the compiled factory keymap."""
import argparse
import hashlib
import copy
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAMES = {
    "Key Press": ("kp", 1), "Home Row Mods Left": ("hrm_left", 2),
    "Home Row Mods Right": ("hrm_right", 2), "Layer-Tap": ("lt", 2),
    "None": ("none", 0), "To Layer": ("to", 1),
    "Mouse Key Press": ("mkp", 1), "Mouse Speed (%)": ("mouse_speed", 1),
    "Bluetooth": ("bt", 2), "Output Selection": ("out", 1),
    **{f"Mouse {kind.title()} {direction.title()}": (f"mouse_{kind}_{direction}", 0)
       for kind in ("move", "scroll") for direction in ("left", "right", "up", "down")},
}
NODE_NAMES = ["default_layer", "nav_layer", "number_layer", "function_layer", "mouse_layer",
              "shifted_number_layer"]


def generate(seed):
    if seed["physicalLayout"]["keyCount"] != 36:
        raise ValueError("Factory physical layout must contain 36 keys")
    mapping = {0: ("none", 0)}
    origin = []
    for behavior in seed["behaviors"]:
        name = behavior["displayName"]
        if name not in NAMES or behavior["id"] in mapping:
            raise ValueError(f"Unmappable/duplicate device-local behavior: {behavior}")
        mapping[behavior["id"]] = NAMES[name]
        origin.append({**behavior, "compiledBehavior": NAMES[name][0]})
    lines = []
    layers = sorted(seed["layers"], key=lambda layer: layer["id"])
    if [layer["id"] for layer in layers] != list(range(5)):
        raise ValueError("Factory layer IDs must be exactly 0..4")
    shifted = copy.deepcopy(layers[2])
    shifted.update(id=5, name="Number + Shift")
    for b in shifted["bindings"][:36]:
        if mapping[b["behaviorId"]][0] == "kp":
            b["param1"] |= 0x02000000
    layers.append(shifted)
    for layer in layers:
        if any(any(b.values()) for b in layer["bindings"][36:]):
            raise ValueError("Nonempty physical-layout padding cannot be migrated")
        if len(layer["bindings"]) < 36:
            raise ValueError("Missing physical bindings")
        lines += [f'                {NODE_NAMES[layer["id"]]} {{',
                  f'                    display-name = {json.dumps(layer["name"])};',
                  '                    bindings = <']
        for b in layer["bindings"][:36]:
            if b["behaviorId"] not in mapping:
                raise ValueError(f"Unmapped binding {b}")
            node, cells = mapping[b["behaviorId"]]
            params = [b["param1"], b["param2"]]
            if any(params[cells:]):
                raise ValueError(f"Unexpected unused parameters {b}")
            if node in ("lt", "to") and params[0] not in range(6):
                raise ValueError(f"Invalid layer reference {b}")
            lines.append("                        &" + node +
                         "".join(f" {p}" for p in params[:cells]))
        lines += ["                    >;", "                };"]
    return "\n".join(lines), origin


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    path = ROOT / "corne-keymap.json"
    content, mapping = generate(json.loads(path.read_text()))
    keymap_path = ROOT / "config" / "corne.keymap"
    keymap = keymap_path.read_text()
    start = keymap.index("                default_layer {")
    end = keymap.index("                extra_3 {", start)
    result = keymap[:start] + content + "\n\n" + keymap[end:]
    if args.check:
        if keymap != result:
            raise SystemExit("Factory keymap differs from current JSON; regenerate")
    else:
        keymap_path.write_text(result)
        (ROOT / "config" / "factory-origin.json").write_text(json.dumps({
            "schema": 1, "seed": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "behaviorMapping": mapping,
            "layerMapping": [{"originId": layer["id"], "compiledId": layer["id"],
                              "name": layer["name"]} for layer in
                             sorted(json.loads(path.read_text())["layers"],
                                    key=lambda layer: layer["id"])],
            "physicalPositionMapping": list(range(36)),
            "removedPaddingPositions": list(range(36, 42)),
            "unsupportedNativeMacros": ["mouse_" + kind + "_" + direction
                for kind in ("move", "scroll") for direction in ("left", "right", "up", "down")],
            "reason": "pause-for-release is not representable in runtime macro format v1",
        }, indent=2) + "\n")


if __name__ == "__main__":
    main()
