import copy
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FactoryMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generator = load_tool("factory_seed")
        cls.seed = json.loads((ROOT / "corne-keymap.json").read_text())

    def test_current_factory_is_generated_and_uses_named_behaviors(self):
        generated, mapping = self.generator.generate(self.seed)
        self.assertIn(generated, (ROOT / "config" / "corne.keymap").read_text())
        self.assertEqual(len(mapping), len(self.seed["behaviors"]))
        self.assertIn("&mouse_speed 400", generated)
        self.assertNotIn("behaviorId", generated)

    def test_unmappable_and_duplicate_ids_rejected(self):
        for mutation in ("unknown-name", "duplicate-id", "unknown-binding"):
            seed = copy.deepcopy(self.seed)
            if mutation == "unknown-name":
                seed["behaviors"][0]["displayName"] = "Guessed Behavior"
            elif mutation == "duplicate-id":
                seed["behaviors"][1]["id"] = seed["behaviors"][0]["id"]
            else:
                seed["layers"][0]["bindings"][0]["behaviorId"] = 65534
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.generator.generate(seed)

    def test_nonempty_padding_unused_parameters_and_bad_layers_rejected(self):
        for mutation in ("padding", "unused-parameter", "missing-layer", "layer-reference"):
            seed = copy.deepcopy(self.seed)
            if mutation == "padding":
                seed["layers"][0]["bindings"][40]["param1"] = 1
            elif mutation == "unused-parameter":
                seed["layers"][0]["bindings"][0]["param2"] = 1
            elif mutation == "missing-layer":
                seed["layers"].pop()
            else:
                seed["layers"][0]["bindings"][30]["param1"] = 25
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.generator.generate(seed)

    def test_origin_metadata_matches_unmodified_input(self):
        origin = json.loads((ROOT / "config" / "factory-origin.json").read_text())
        self.assertEqual(origin["sha256"],
                         hashlib.sha256((ROOT / "corne-keymap.json").read_bytes()).hexdigest())
        self.assertEqual(origin["physicalPositionMapping"], list(range(36)))
        self.assertEqual([item["originId"] for item in origin["layerMapping"]], list(range(5)))
        self.assertEqual(len(origin["unsupportedNativeMacros"]), 8)

    def test_combos_have_single_runtime_owner_and_native_macros_retained(self):
        keymap = (ROOT / "config" / "corne.keymap").read_text()
        self.assertNotIn('compatible = "zmk,combos"', keymap)
        self.assertEqual(keymap.count('compatible = "cormoran,runtime-combo-defaults"'), 1)
        self.assertEqual(keymap.count("<&macro_pause_for_release>"), 8)

    def test_saved_identity_policy_is_explicit_and_crc_upgrade_rejected(self):
        snippet = (ROOT / "snippets" / "studio-viewer-usb" / "studio-viewer-usb.conf").read_text()
        self.assertIn("CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE=y", snippet)
        self.assertIn("CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE=160", snippet)
        safety = (ROOT / "viewer" / "runtime_safety.c").read_text()
        self.assertIn("BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE)",
                      safety)

    def test_runtime_rpc_capacity_exceeds_legacy_uart_buffer(self):
        snippet = (ROOT / "snippets" / "studio-viewer-usb" / "studio-viewer-usb.conf").read_text()
        self.assertIn("CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=512", snippet)
        self.assertIn("CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE=8192", snippet)

    def test_viewer_omits_vacant_runtime_combo_slots(self):
        viewer = (ROOT / "viewer" / "viewer.c").read_text()
        export = viewer[viewer.index("for (uint32_t slot = 0; slot < zmk_runtime_combo_count();"):
                        viewer.index("zmk_runtime_macro_for_each(viewer_macro")]
        self.assertIn("if (ret || !combo.key_position_len)", export)


class RuntimeAdaptationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patcher = load_tool("patch_runtime")

    def test_checked_replacement_refuses_drift(self):
        with self.assertRaises(ValueError):
            self.patcher.replace_once("changed", "expected", "new")
        with self.assertRaises(ValueError):
            self.patcher.replace_once("expected expected", "expected", "new")

    def patched_defaults(self):
        return self.patcher.patch_defaults("""    /* Seed-if-absent: old behavior */
    ret = zmk_runtime_macro_create(cfg->name, encoded, size, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
                                   NULL);
static int runtime_macro_seed_dt_defaults(void) {
    for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {
        install_one_default(runtime_macro_default_configs[i]);
    }

    return 0;
}
""")

    def test_defaults_persist_once_and_do_not_resurrect(self):
        result = self.patched_defaults()
        self.assertIn("ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST", result)
        self.assertIn('settings_save_one("runtime_macro_factory/v1"', result)
        self.assertIn("if (factory_seeded)", result)
        self.assertNotIn("ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY", result)
        self.assertIn("if (factory_deleted[i])", result)

    def test_defaults_and_discard_reference_failure_safety(self):
        compiler = os.environ.get("CC") or next(
            (path for name in ("cc", "gcc", "clang", "cl") if (path := shutil.which(name))),
            None,
        )
        if not compiler:
            self.skipTest("Host C compiler required")
        result = self.patched_defaults()
        logic = result[result.index("static uint8_t factory_seeded;"):]
        safety = (ROOT / "viewer" / "runtime_safety.c").read_text()
        start = safety.index("static bool discard_removes_referenced_macro(")
        discard = safety[start:safety.index("\nint __real_zmk_runtime_macro_delete", start)]
        start = safety.index("struct reset_scope {")
        scope = safety[start:safety.index("\nint __real_zmk_runtime_macro_rename", start)]
        template = (ROOT / "tests" / "runtime_factory_test.c").read_text()
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            source = Path(directory) / "factory.c"
            executable = Path(directory) / ("factory.exe" if os.name == "nt" else "factory")
            source.write_text(template.replace("/* FACTORY_LOGIC */", logic)
                              .replace("/* DISCARD_LOGIC */", discard)
                              .replace("/* SCOPE_LOGIC */", scope))
            if Path(compiler).name.lower() in ("cl", "cl.exe"):
                command = [compiler, "/nologo", "/W4", "/WX", "/std:c11", str(source),
                           f"/Fe:{executable}", f"/Fo:{Path(directory) / 'factory.obj'}"]
            else:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-pedantic", str(source), "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_rename_preserves_ordinal_and_has_bounded_staging(self):
        result = self.patcher.patch_keyspace("")
        self.assertIn("CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE", result)
        self.assertIn("other && other != setting ? -EEXIST", result)
        self.assertIn("keyspace_write_raw_payload_with_key(setting, new_key", result)
        self.assertNotIn("keyspace_bind_slot_locked", result)
        self.assertNotIn("keyspace_release_slot_locked", result)

    def test_removed_saved_behavior_ids_are_not_recycled(self):
        fixture = ("        name[len] = '\\0';\n"
                   "        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {\n"
                   "        item->local_id = ++largest_local_id;")
        result = self.patcher.patch_behavior(fixture)
        self.assertLess(result.index("largest_local_id = MAX"), result.index("STRUCT_SECTION"))
        self.assertIn("largest_local_id == UINT16_MAX", result)
        self.assertIn("return -ENOSPC", result)

    def test_saved_bindings_validate_after_id_table_load(self):
        fixture = """static int value_from_storage(
    const struct zmk_custom_setting *setting, const struct zmk_custom_setting_value *data) {
    struct zmk_custom_setting_value value = *data;
    int ret = zmk_custom_setting_validate(setting, &value);
    if (ret < 0) {
        return ret;
    }
    return store_memory_value_locked(setting, &value);
}
/* Length of the user-key */
static bool custom_settings_initialized;
static int custom_settings_handle_commit(void) {
    if (custom_settings_initialized) {
        return 0;
    }
    custom_settings_initialized = true;
    raise_zmk_custom_settings_initialized((struct zmk_custom_settings_initialized){0});
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(custom_settings, SETTINGS_SUBTREE, NULL, custom_settings_handle_set,
                               custom_settings_handle_commit, NULL);
"""
        patched = self.patcher.patch_custom_settings(fixture)
        self.assertIn("SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(", patched)
        self.assertIn("custom_settings_handle_commit, NULL, 1)", patched)
        compiler = os.environ.get("CC") or next(
            (path for name in ("cc", "gcc", "clang", "cl") if (path := shutil.which(name))),
            None,
        )
        if not compiler:
            self.skipTest("Host C compiler required")
        load = patched[:patched.index("/* Length of the user-key")]
        commit = patched[patched.index("static int validate_loaded_behaviors"):
                         patched.index("/* Commit after behavior/local_id")]
        template = (ROOT / "tests" / "runtime_settings_load_test.c").read_text()
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            source = Path(directory) / "settings.c"
            executable = Path(directory) / ("settings.exe" if os.name == "nt" else "settings")
            source.write_text(template.replace("/* LOAD_LOGIC */", load)
                              .replace("/* COMMIT_LOGIC */", commit))
            if Path(compiler).name.lower() in ("cl", "cl.exe"):
                command = [compiler, "/nologo", "/W4", "/WX", "/std:c11", str(source),
                           f"/Fe:{executable}", f"/Fo:{Path(directory) / 'settings.obj'}"]
            else:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-pedantic", str(source), "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_combo_rpc_suppresses_only_dispatch_notifications(self):
        fixture = """    if (!decode_ok) {
        return true;
    }
    int ret = 0;
    switch (req.which_request_type) {
    default:
        ret = -ENOTSUP;
        break;
    }
    if (ret < 0) {
        set_errno_error(resp, "Runtime combo request", ret);
    }
    return true;
"""
        result = self.patcher.patch_combo_handler(fixture)
        self.assertEqual(result.count("zmk_custom_settings_notify_suppress_begin();"), 1)
        self.assertEqual(result.count("zmk_custom_settings_notify_suppress_end();"), 1)
        self.assertLess(result.index("return true;"),
                        result.index("zmk_custom_settings_notify_suppress_begin"))
        self.assertLess(result.index("zmk_custom_settings_notify_suppress_begin"),
                        result.index("switch (req.which_request_type)"))
        self.assertLess(result.index("zmk_custom_settings_notify_suppress_end"),
                        result.index("if (ret < 0)"))


if __name__ == "__main__":
    unittest.main()
