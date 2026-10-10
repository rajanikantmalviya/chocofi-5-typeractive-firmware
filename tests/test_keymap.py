import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = json.loads((ROOT / "Corne-keymap.json").read_text())
KEYMAP = (ROOT / "config" / "corne.keymap").read_text()
LAYERS = {"BASE": 0, "NAV": 1, "NUMBER": 2, "FUNC": 3, "NUMBER_SHIFT": 4}
KEYS = {chr(ord("A") + i): 0x70004 + i for i in range(26)}
KEYS.update({f"N{i}": 0x7001D + i for i in range(1, 10)})
KEYS["N0"] = 0x70027
KEYS.update({f"F{i}": 0x70039 + i for i in range(1, 13)})
KEYS.update(
    {
        name: 0x70000 + usage
        for name, usage in {
            "RET": 0x28,
            "ESC": 0x29,
            "BSPC": 0x2A,
            "TAB": 0x2B,
            "SPACE": 0x2C,
            "EQUAL": 0x2E,
            "LBKT": 0x2F,
            "RBKT": 0x30,
            "BSLH": 0x31,
            "SEMI": 0x33,
            "SQT": 0x34,
            "GRAVE": 0x35,
            "COMMA": 0x36,
            "DOT": 0x37,
            "FSLH": 0x38,
            "HOME": 0x4A,
            "PG_UP": 0x4B,
            "DEL": 0x4C,
            "END": 0x4D,
            "PG_DN": 0x4E,
            "RIGHT": 0x4F,
            "LEFT": 0x50,
            "DOWN": 0x51,
            "UP": 0x52,
            "LCTRL": 0xE0,
            "LSHIFT": 0xE1,
            "LALT": 0xE2,
            "LGUI": 0xE3,
            "RCTRL": 0xE4,
            "RSHIFT": 0xE5,
            "RALT": 0xE6,
            "RGUI": 0xE7,
        }.items()
    }
)
KEYS.update(
    {
        "C_BRI_DN": 0xC0070,
        "C_BRI_UP": 0xC006F,
        "C_AL_CALC": 0xC0192,
        "C_MUTE": 0xC00E2,
        "C_VOL_DN": 0xC00EA,
        "C_VOL_UP": 0xC00E9,
        "LCLK": 1,
        "RCLK": 2,
        "OUT_USB": 1,
        "OUT_BLE": 2,
    }
)
BEHAVIORS = {
    "kp": "Key Press",
    "hrm": "Home Row Mods",
    "hrm_left": "Home Row Mods",
    "hrm_right": "Home Row Mods",
    "lt": "Layer-Tap",
    "none": "None",
    "trans": "Transparent",
    "mkp": "Mouse Key Press",
    "bt": "Bluetooth",
    "out": "Output Selection",
}
for category in ("move", "scroll"):
    for direction in ("left", "right", "up", "down"):
        BEHAVIORS[f"mouse_{category}_{direction}"] = (
            f"Mouse {category.title()} {direction.title()}"
        )


def node_body(name):
    match = re.search(r"\b" + name + r"\s*\{", KEYMAP)
    if match is None:
        raise AssertionError(f"Missing node: {name}")
    depth = 1
    for index in range(match.end(), len(KEYMAP)):
        if KEYMAP[index] == "{":
            depth += 1
        elif KEYMAP[index] == "}":
            depth -= 1
            if depth == 0:
                return KEYMAP[match.end():index]
    raise AssertionError(f"Unclosed node: {name}")


def bindings(body):
    match = re.search(r"\bbindings\s*=\s*((?:<[^>]*>\s*,?\s*)+);", body)
    if match is None:
        raise AssertionError("Missing bindings")
    return [
        (name, params.split())
        for name, params in re.findall(r"&(\w+)([^&<>]*)", match.group(1))
    ]


def value(token):
    shifted = re.fullmatch(r"LS\((\w+)\)", token)
    if shifted:
        return KEYS[shifted.group(1)] | 0x2000000
    if token in LAYERS:
        return LAYERS[token]
    return int(token) if token.isdigit() else KEYS[token]


def encoded_binding(binding):
    behavior, tokens = binding
    params = []
    for token in tokens:
        if token in {"BT_CLR": 0, "BT_NXT": 1, "BT_PRV": 2}:
            params.extend([{"BT_CLR": 0, "BT_NXT": 1, "BT_PRV": 2}[token], 0])
        else:
            params.append(value(token))
    if len(params) > 2:
        raise AssertionError(f"Too many parameters: {binding}")
    return BEHAVIORS[behavior], params + [0] * (2 - len(params))


class KeymapTests(unittest.TestCase):
    def test_seed_layers(self):
        self.assertEqual(SEED["format"], "zmk-studio-keymap")
        self.assertEqual(SEED["version"], 1)
        self.assertEqual(SEED["physicalLayout"]["keyCount"], 36)
        names = {item["id"]: item["displayName"] for item in SEED["behaviors"]}
        nodes = ["default_layer", "nav_layer", "number_layer", "function_layer"]
        self.assertEqual(len(SEED["layers"]), len(nodes))
        for layer, node in zip(SEED["layers"], nodes):
            body = node_body(node)
            self.assertIn(f'display-name = "{layer["name"]}";', body)
            actual = bindings(body)
            self.assertEqual(len(actual), 36)
            for index, (binding, expected) in enumerate(
                zip(actual, layer["bindings"][:36])
            ):
                with self.subTest(layer=layer["name"], position=index):
                    if expected["behaviorId"] == 0:
                        self.assertEqual(expected["param1"], 0)
                        self.assertEqual(expected["param2"], 0)
                        self.assertEqual(binding, ("none", []))
                    else:
                        self.assertEqual(
                            encoded_binding(binding),
                            (
                                names[expected["behaviorId"]],
                                [expected["param1"], expected["param2"]],
                            ),
                        )
            for padding in layer["bindings"][36:]:
                self.assertEqual(
                    padding, {"behaviorId": 0, "param1": 0, "param2": 0}
                )

    def test_layer_order(self):
        keymap = node_body("keymap")
        self.assertEqual(
            re.findall(r"^\s*(\w+)\s*\{", keymap, re.M),
            [
                "default_layer",
                "nav_layer",
                "number_layer",
                "function_layer",
                "shifted_number_layer",
                "extra_3",
            ],
        )
        for name, number in LAYERS.items():
            self.assertRegex(KEYMAP, rf"(?m)^#define {name} {number}$")
        self.assertEqual(KEYMAP.count('status = "reserved";'), 1)

    def test_shifted_number_mirrors_seed(self):
        normal = bindings(node_body("number_layer"))
        shifted = bindings(node_body("shifted_number_layer"))
        self.assertEqual(len(shifted), 36)
        for original, modified in zip(normal, shifted):
            with self.subTest(binding=original):
                if original[0] == "none":
                    self.assertEqual(modified, original)
                else:
                    self.assertEqual(original[0], "kp")
                    self.assertEqual(
                        encoded_binding(modified),
                        ("Key Press", [value(original[1][0]) | 0x2000000, 0]),
                    )

    def test_combos(self):
        expected = {
            "combo_backspace": ([6, 7], ["BASE"], [("kp", ["BSPC"])]),
            "combo_delete": ([7, 8], ["BASE"], [("kp", ["DEL"])]),
            "combo_enter_right": ([16, 17, 18], ["BASE"], [("kp", ["RET"])]),
            "combo_space_right": ([16, 17], ["BASE"], [("kp", ["SPACE"])]),
            "combo_space_left": ([12, 13], ["BASE"], [("kp", ["SPACE"])]),
            "combo_enter_left": ([11, 12, 13], ["BASE"], [("kp", ["RET"])]),
            "combo_escape": ([0, 1], ["BASE"], [("kp", ["ESC"])]),
            "combo_tab": ([1, 2], ["BASE"], [("kp", ["TAB"])]),
            "combo_number_er": ([2, 3], ["BASE", "NUMBER"], [("tog", ["NUMBER"])]),
            "combo_function": ([1, 2, 3], ["BASE"], [("td_func", [])]),
            "combo_shift": ([10, 11], list(LAYERS), [("kt", ["LSHIFT"])]),
        }
        self.assertEqual(
            set(re.findall(r"\b(combo_\w+)\s*\{", KEYMAP)), set(expected)
        )
        self.assertNotRegex(KEYMAP, r"\bcombo_number\b")
        for name, (positions, layers, actions) in expected.items():
            with self.subTest(combo=name):
                body = node_body(name)
                self.assertIn("timeout-ms = <80>;", body)
                self.assertEqual(
                    [int(n) for n in re.search(r"key-positions = <([^>]+)>", body)[1].split()],
                    positions,
                )
                self.assertEqual(
                    re.search(r"layers = <([^>]+)>", body)[1].split(), layers
                )
                self.assertEqual(bindings(body), actions)

    def test_tap_dances(self):
        expected = {
            "function_tap_dance": [("sl", ["FUNC"])],
        }
        self.assertEqual(KEYMAP.count('compatible = "zmk,behavior-tap-dance";'), 1)
        for removed in ("td_number", "td_number_shift", "td_shift"):
            self.assertNotRegex(KEYMAP, rf"\b{removed}\b")
        for name, actions in expected.items():
            with self.subTest(dance=name):
                body = node_body(name)
                self.assertIn('compatible = "zmk,behavior-tap-dance";', body)
                self.assertIn("#binding-cells = <0>;", body)
                self.assertIn("tapping-term-ms = <200>;", body)
                self.assertEqual(bindings(body), actions)

    def test_overlapping_combos(self):
        for shorter, longer in (
            ("combo_space_right", "combo_enter_right"),
            ("combo_space_left", "combo_enter_left"),
            ("combo_tab", "combo_function"),
            ("combo_number_er", "combo_function"),
        ):
            with self.subTest(shorter=shorter, longer=longer):
                bodies = [node_body(shorter), node_body(longer)]
                positions = [
                    set(re.search(r"key-positions = <([^>]+)>", body)[1].split())
                    for body in bodies
                ]
                self.assertLess(positions[0], positions[1])
                for body in bodies:
                    self.assertIn("timeout-ms = <80>;", body)
                    self.assertIn(
                        "BASE", re.search(r"layers = <([^>]+)>", body)[1].split()
                    )

    def test_home_row_mod_timing(self):
        for name in ("home_row_mods", "home_row_mods_left", "home_row_mods_right"):
            with self.subTest(behavior=name):
                body = node_body(name)
                self.assertIn("tapping-term-ms = <300>;", body)
                self.assertIn("require-prior-idle-ms = <175>;", body)
                self.assertIn("quick-tap-ms = <175>;", body)
                self.assertIn('flavor = "balanced";', body)
                self.assertEqual(bindings(body), [("kp", []), ("kp", [])])

    def test_opposite_hand_home_row_mods(self):
        left_positions = set(range(0, 5)) | set(range(10, 15)) | set(range(20, 25)) | {30, 31, 32}
        right_positions = set(range(36)) - left_positions
        for hand, triggers in (("left", right_positions), ("right", left_positions)):
            with self.subTest(hand=hand):
                body = node_body(f"home_row_mods_{hand}")
                self.assertIn(f'display-name = "Home Row Mods {hand.title()}";', body)
                self.assertIn("hold-trigger-on-release;", body)
                actual = re.search(r"hold-trigger-key-positions = <([^>]+)>", body)[1]
                self.assertEqual([int(position) for position in actual.split()], sorted(triggers))
        base = bindings(node_body("default_layer"))
        for position in (10, 11, 12, 13):
            self.assertEqual(base[position][0], "hrm_left")
        for position in (16, 17, 18, 19):
            self.assertEqual(base[position][0], "hrm_right")
        self.assertNotIn("hold-trigger-key-positions", node_body("home_row_mods"))


if __name__ == "__main__":
    unittest.main()
