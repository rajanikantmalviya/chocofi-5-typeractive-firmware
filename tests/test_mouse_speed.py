import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MouseSpeedTests(unittest.TestCase):
    def test_scaling_math(self):
        compiler = os.environ.get("CC") or next(
            (path for name in ("cc", "gcc", "clang", "cl")
             if (path := shutil.which(name))),
            None,
        )
        if compiler is None:
            self.skipTest("A host C compiler is required for the mouse speed math tests")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ("mouse-speed-test.exe" if os.name == "nt"
                                            else "mouse-speed-test")
            source = ROOT / "tests" / "mouse_speed_math_test.c"
            include = ROOT / "mouse"
            if Path(compiler).name.lower() in ("cl", "cl.exe"):
                command = [compiler, "/nologo", "/W4", "/WX", "/std:c11",
                           f"/I{include}", str(source), f"/Fe:{executable}",
                           f"/Fo:{Path(directory) / 'mouse-speed-test.obj'}"]
            else:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-pedantic", f"-I{include}", str(source), "-o", str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_studio_behavior_and_cursor_only_wiring(self):
        nodes = (ROOT / "dts" / "behaviors" / "mouse_speed.dtsi").read_text()
        self.assertIn('display-name = "Mouse Speed (%)";', nodes)
        self.assertIn("#binding-cells = <1>;", nodes)
        self.assertIn("&mmv_input_listener {", nodes)
        self.assertNotIn("&msc_input_listener", nodes)
        keymap = (ROOT / "config" / "corne.keymap").read_text()
        self.assertIn("#include <behaviors/mouse_speed.dtsi>", keymap)
        self.assertNotRegex(keymap, r"&mouse_speed\s+\d")
        driver = (ROOT / "mouse" / "mouse_speed.c").read_text()
        self.assertIn("BEHAVIOR_LOCALITY_CENTRAL", driver)
        self.assertIn("ATOMIC_INIT(100)", driver)
        self.assertIn(".range = {.min = 1, .max = INT32_MAX}", driver)
        self.assertNotIn("BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE", driver)
        self.assertIn("INPUT_REL_X", driver)
        self.assertIn("INPUT_REL_Y", driver)
        self.assertNotIn("INPUT_REL_WHEEL", driver)
        self.assertNotIn("time_to_max_speed", driver)


if __name__ == "__main__":
    unittest.main()
