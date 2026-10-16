import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class RuntimeCodecTests(unittest.TestCase):
    def test_macro_reference_decoder(self):
        compiler = os.environ.get("CC") or next(
            (path for name in ("cc", "gcc", "clang", "cl") if (path := shutil.which(name))),
            None,
        )
        if not compiler:
            self.skipTest("Host C compiler required")
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            executable = Path(directory) / ("codec.exe" if os.name == "nt" else "codec")
            source = ROOT / "tests" / "runtime_codec_test.c"
            include = ROOT / "viewer"
            if Path(compiler).name.lower() in ("cl", "cl.exe"):
                command = [compiler, "/nologo", "/W4", "/WX", "/std:c11", f"/I{include}",
                           str(source), f"/Fe:{executable}", f"/Fo:{Path(directory) / 'codec.obj'}"]
            else:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           f"-I{include}", str(source), "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
