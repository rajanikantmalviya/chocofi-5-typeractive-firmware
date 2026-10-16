"""Checked, build-local adaptations for the exact pinned community revision."""
import argparse
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Pinned runtime source changed; adaptation must be reviewed")
    return text.replace(old, new, 1)


def patch_player(text):
    text = replace_once(text,
        "    return zmk_custom_setting_keyspace_delete(&runtime_macros, key);\n}",
        """    extern int runtime_macro_factory_note_delete(const char *name);
    ret = runtime_macro_factory_note_delete(name);
    return ret < 0 ? ret : zmk_custom_setting_keyspace_delete(&runtime_macros, key);
}""")
    text = replace_once(text,
        "    ret = queue_runtime_macro(player.items, count, &event, &queued_duration_ms);",
        """    extern void runtime_safety_read_lock(void);
    extern void runtime_safety_event_unlock(void);
    runtime_safety_read_lock();
    ret = queue_runtime_macro(player.items, count, &event, &queued_duration_ms);
    runtime_safety_event_unlock();""")
    start = text.index("int zmk_runtime_macro_rename(")
    end = text.index("\nint zmk_runtime_macro_name_for_slot(", start)
    text = text[:start] + """extern int runtime_keyspace_rename_in_place(
    const struct zmk_custom_setting *setting, const char *new_key,
    enum zmk_custom_setting_write_mode mode);

int zmk_runtime_macro_rename(const char *old_name, const char *new_name,
                             enum zmk_custom_setting_write_mode mode) {
    char old_key[RUNTIME_MACRO_MAX_KEY_LEN];
    char new_key[RUNTIME_MACRO_MAX_KEY_LEN];
    int ret = build_key(old_name, old_key, sizeof(old_key));
    if (ret < 0) {
        return ret;
    }
    ret = build_key(new_name, new_key, sizeof(new_key));
    if (ret < 0 || !new_name[0]) {
        return ret < 0 ? ret : -EINVAL;
    }
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_keyspace_find(&runtime_macros, old_key);
    if (!setting) {
        return -ENOENT;
    }
    if (!strcmp(old_name, new_name)) {
        return 0;
    }
    extern int runtime_macro_factory_note_delete(const char *name);
    ret = runtime_macro_factory_note_delete(old_name);
    return ret < 0 ? ret : runtime_keyspace_rename_in_place(setting, new_key, mode);
}
""" + text[end:]
    text = replace_once(text,
        "    uint32_t pending_delay_ms = 0;\n    *queued_duration_ms = 0;",
        """    extern struct k_msgq zmk_behavior_queue_msgq;
    /* Worst-case two queue entries per tap plus one trailing delay.
     * Nothing is emitted if the shared native queue cannot fit the entire macro. */
    if (k_msgq_num_free_get(&zmk_behavior_queue_msgq) < count * 2 + 1) {
        return -ENOSPC;
    }
    uint32_t pending_delay_ms = 0;
    *queued_duration_ms = 0;""")
    return text + """

bool runtime_macro_factory_active(void) {
    k_mutex_lock(&player.lock, K_FOREVER);
    bool active = player.active;
    k_mutex_unlock(&player.lock);
    return active;
}
"""

def patch_keyspace(text):
    return text + """

int runtime_keyspace_discard_created(const struct zmk_custom_setting *setting) {
    if (!setting || !setting->_keyspace) {
        return -EINVAL;
    }
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT ? -EINVAL : 0;
    if (!ret) {
        struct zmk_custom_setting_keyspace *space =
            (struct zmk_custom_setting_keyspace *)setting->_keyspace;
        keyspace_release_slot_for_setting_locked(space, setting);
    }
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

/* Keep the ordinal descriptor/storage identity unchanged during rename. */
int runtime_keyspace_rename_in_place(const struct zmk_custom_setting *setting,
                                    const char *new_key,
                                    enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting_keyspace *space = setting->_keyspace;
    if (!space || !keyspace_key_matches_prefix(space, new_key) ||
        strlen(new_key) >= space->max_key_len) {
        return -EINVAL;
    }
    uint8_t payload[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
    size_t size = 0;
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    const struct zmk_custom_setting *other = zmk_custom_setting_keyspace_find(space, new_key);
    int ret = other && other != setting ? -EEXIST :
        keyspace_read_into(setting, payload, sizeof(payload), &size, NULL);
    if (!ret) {
        ret = keyspace_write_raw_payload_with_key(setting, new_key, payload, size, mode);
    }
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}
"""

def patch_behavior(text):
    text = replace_once(text,
        "        name[len] = '\\0';\n"
        "        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {",
        """        name[len] = '\\0';
        /* Never recycle a persisted ID whose old behavior is no longer compiled. */
        largest_local_id = MAX(largest_local_id, local_id);
        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {""")
    return replace_once(text,
        "        item->local_id = ++largest_local_id;",
        """        if (largest_local_id == UINT16_MAX) {
            LOG_ERR("Saved behavior ID table exhausted; refusing ID reuse");
            return -ENOSPC;
        }
        item->local_id = ++largest_local_id;""")


def patch_custom_settings(text):
    text = replace_once(text,
        "static bool custom_settings_initialized;\n",
        "")
    start = text.index("static int value_from_storage(")
    end = text.index("\n/* Length of the user-key", start)
    load = replace_once(text[start:end],
        "    int ret = zmk_custom_setting_validate(setting, &value);",
        """    /* SETTINGS_TABLE IDs may appear later in the same boot load. */
    int ret = !custom_settings_initialized &&
                      setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR
                  ? 0 : zmk_custom_setting_validate(setting, &value);""")
    text = text[:start] + "static bool custom_settings_initialized;\n\n" + load + text[end:]
    text = replace_once(text,
        "static int custom_settings_handle_commit(void) {",
        """static int validate_loaded_behaviors(void) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
            continue;
        }
        uint32_t count = zmk_custom_setting_is_array(setting)
                             ? setting->array_state->size : 1;
        for (uint32_t index = 0; index < count; index++) {
            bool persisted = zmk_custom_setting_is_array(setting)
                                 ? setting->array_state->has_persistent[index]
                                 : setting_has_persistent_value(setting);
            if (!persisted) {
                continue;
            }
            struct zmk_custom_setting_value value = {
                .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
                .behavior_value = setting->state->behavior,
            };
            if (zmk_custom_setting_is_array(setting)) {
                value = setting->array_state->values[index];
                /* Sparse arrays persist their compiled empty placeholders too. */
                if (value_equals(&value, &setting->array_state->defaults[index])) {
                    continue;
                }
            }
            int ret = zmk_custom_setting_validate(setting, &value);
            if (ret < 0) {
                LOG_ERR("Invalid saved behavior for %s/%s[%u]: %d",
                        setting->custom_subsystem_id, setting->key, index, ret);
                return ret;
            }
        }
    }
    return 0;
}

static int custom_settings_handle_commit(void) {""")
    text = replace_once(text,
        "    custom_settings_initialized = true;\n"
        "    raise_zmk_custom_settings_initialized",
        """    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = validate_loaded_behaviors();
    k_mutex_unlock(&custom_settings_lock);
    if (ret < 0) {
        return ret;
    }
    custom_settings_initialized = true;
    raise_zmk_custom_settings_initialized""")
    return replace_once(text,
        "SETTINGS_STATIC_HANDLER_DEFINE(custom_settings, SETTINGS_SUBTREE, NULL, custom_settings_handle_set,\n"
        "                               custom_settings_handle_commit, NULL);",
        """/* Commit after behavior/local_id has loaded and assigned any new IDs. */
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(
    custom_settings, SETTINGS_SUBTREE, NULL, custom_settings_handle_set,
    custom_settings_handle_commit, NULL, 1);""")


def patch_combo_handler(text):
    text = replace_once(text,
        "    int ret = 0;\n    switch (req.which_request_type) {",
        """    int ret = 0;
    /* Match the macro handler: the RPC reply already confirms local edits. */
    zmk_custom_settings_notify_suppress_begin();
    switch (req.which_request_type) {""")
    return replace_once(text,
        "    if (ret < 0) {\n"
        "        set_errno_error(resp, \"Runtime combo request\", ret);",
        """    zmk_custom_settings_notify_suppress_end();
    if (ret < 0) {
        set_errno_error(resp, "Runtime combo request", ret);""")


def patch_defaults(text):
    text = replace_once(text,
        "ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,\n                                   NULL);",
        "ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,\n                                   NULL);")
    start = text.index("    /* Seed-if-absent:")
    end = text.index("    ret = zmk_runtime_macro_create(", start)
    text = text[:start] + "    /* Persist once; ordinal storage keeps default slots stable. */\n" + text[end:]
    text = replace_once(text,
        "static int runtime_macro_seed_dt_defaults(void) {\n"
        "    for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {\n"
        "        install_one_default(runtime_macro_default_configs[i]);\n"
        "    }\n\n"
        "    return 0;\n"
        "}",
        """static uint8_t factory_seeded;
static uint8_t factory_deleted[RUNTIME_MACRO_DEFAULT_COUNT];
BUILD_ASSERT(RUNTIME_MACRO_DEFAULT_COUNT <= CONFIG_ZMK_RUNTIME_MACRO_COUNT,
             "Factory macro defaults exceed bounded macro slots");

static int factory_marker_set(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (len != 1) {
        return -EINVAL;
    }
    uint8_t *destination = !strcmp(name, "v1") ? &factory_seeded : NULL;
    if (!strncmp(name, "deleted/", 8)) {
        for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {
            if (!strcmp(name + 8, runtime_macro_default_configs[i]->name)) {
                destination = &factory_deleted[i];
                break;
            }
        }
    }
    if (!destination) {
        return -EINVAL;
    }
    return read(arg, destination, 1) == 1 ? 0 : -EIO;
}

SETTINGS_STATIC_HANDLER_DEFINE(runtime_macro_factory_marker, "runtime_macro_factory",
                               NULL, factory_marker_set, NULL, NULL);

static int runtime_macro_seed_dt_defaults(void) {
    if (factory_seeded) {
        return 0;
    }
    for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {
        if (factory_deleted[i]) {
            continue;
        }
        int ret = install_one_default(runtime_macro_default_configs[i]);
        if (ret) {
            return ret;
        }
    }
    uint8_t marker = 1;
    int ret = settings_save_one("runtime_macro_factory/v1", &marker, sizeof(marker));
    if (!ret) {
        factory_seeded = marker;
    }
    return ret;
}""")
    return text + """

int runtime_macro_factory_note_delete(const char *name) {
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
    for (size_t i = 0; i < RUNTIME_MACRO_DEFAULT_COUNT; i++) {
        if (strcmp(name, runtime_macro_default_configs[i]->name)) {
            continue;
        }
        if (factory_deleted[i]) {
            return 0;
        }
        char key[96];
        int length = snprintf(key, sizeof(key), "runtime_macro_factory/deleted/%s", name);
        if (length < 0 || (size_t)length >= sizeof(key)) {
            return -ENAMETOOLONG;
        }
        uint8_t deleted = 1;
        int ret = settings_save_one(key, &deleted, sizeof(deleted));
        if (!ret) {
            factory_deleted[i] = deleted;
        }
        return ret;
    }
#else
    ARG_UNUSED(name);
#endif
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    patch = (patch_defaults if args.source.name.endswith("dt_defaults.c") else
             patch_behavior if args.source.name == "behavior.c" else
             patch_custom_settings if args.source.name == "custom_settings.c" else
             patch_combo_handler if args.source.name == "runtime_combo_handler.c" else
             patch_keyspace if args.source.name == "custom_settings_keyspace.c" else patch_player)
    args.destination.write_text(patch(args.source.read_text()))


if __name__ == "__main__":
    main()
