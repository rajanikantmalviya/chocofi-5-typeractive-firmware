#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RUNTIME_MACRO_DEFAULT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_COUNT 8
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#define SETTINGS_STATIC_HANDLER_DEFINE(...) _Static_assert(1, "handler")
#define DT_HAS_COMPAT_STATUS_OKAY(...) 1
typedef int (*settings_read_cb)(void *, void *, size_t);

struct runtime_macro_default_config { const char *name; };
static const struct runtime_macro_default_config defaults[] = {{"Default A"}, {"Default B"}};
static const struct runtime_macro_default_config *runtime_macro_default_configs[] = {
    &defaults[0], &defaults[1]
};
static uint8_t stored_seed, stored_deleted[2];
static bool live[2], fail_seed, fail_delete;
static unsigned installed;

static int install_one_default(const struct runtime_macro_default_config *cfg) {
    unsigned index = cfg == &defaults[0] ? 0 : 1;
    installed++;
    live[index] = true;
    return 0;
}

static int settings_save_one(const char *key, const void *data, size_t size) {
    assert(size == 1);
    if (!strcmp(key, "runtime_macro_factory/v1")) {
        if (fail_seed) return -EIO;
        stored_seed = *(const uint8_t *)data;
        return 0;
    }
    assert(!strncmp(key, "runtime_macro_factory/deleted/", 30));
    if (fail_delete) return -EIO;
    unsigned index = !strcmp(key + 30, defaults[0].name) ? 0 : 1;
    assert(!strcmp(key + 30, defaults[index].name));
    stored_deleted[index] = *(const uint8_t *)data;
    return 0;
}

/* FACTORY_LOGIC */

#define ZMK_RUNTIME_MACRO_SUBSYSTEM_ID "cormoran__runtime_macro"
#define ZMK_RUNTIME_MACRO_KEY_PREFIX "macro/"
#define ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT 2
#define CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN 64
#define ARG_UNUSED(value) ((void)(value))
#define snprintk snprintf
struct zmk_custom_setting_state { uint8_t flags; };
struct zmk_custom_setting {
    const void *_keyspace;
    const char *custom_subsystem_id, *key;
    struct zmk_custom_setting_state *state;
};
static const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting) {
    return setting->key;
}
static int zmk_runtime_macro_slot_for_name(const char *name, uint32_t *slot) {
    if (strcmp(name, "created")) return -ENOENT;
    *slot = 0;
    return 0;
}
static bool macro_referenced = true, edit_busy;
static bool macro_live = true;
static const struct zmk_custom_setting *active_macro;
static unsigned discards, releases;
static bool referenced(uint32_t slot) { return slot == 0 && macro_referenced; }
static bool mutation_busy(void) { return edit_busy; }
static void runtime_safety_read_lock(void) {}
static void runtime_safety_event_unlock(void) {}
int __real_zmk_custom_setting_discard(const struct zmk_custom_setting *setting) {
    assert(setting);
    discards++;
    return 0;
}
int runtime_keyspace_discard_created(const struct zmk_custom_setting *setting) {
    assert(setting->_keyspace);
    assert(!(setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT));
    releases++;
    macro_live = false;
    return 0;
}

/* DISCARD_LOGIC */

static const struct zmk_custom_setting *zmk_custom_setting_find(
    const char *subsystem, const char *key) {
    return macro_live && active_macro && !strcmp(subsystem, active_macro->custom_subsystem_id) &&
           !strcmp(key, active_macro->key) ? active_macro : NULL;
}
static int zmk_runtime_macro_for_each(
    int (*callback)(uint32_t, const char *, size_t, bool, void *), void *data) {
    return macro_live && active_macro ?
        callback(0, active_macro->key + strlen(ZMK_RUNTIME_MACRO_KEY_PREFIX), 1, false, data) : 0;
}
int __real_zmk_custom_settings_discard_scope(const char *subsystem, const char *key,
                                            const char *prefix, uint32_t *count) {
    *count = 0;
    if (!macro_live || !active_macro ||
        (subsystem && strcmp(subsystem, active_macro->custom_subsystem_id)) ||
        (key && key[0] && strcmp(key, active_macro->key)) ||
        (prefix && prefix[0] && strncmp(prefix, active_macro->key, strlen(prefix)))) {
        return 0;
    }
    /* Match the same-translation-unit call that is not intercepted by --wrap. */
    (*count)++;
    return __real_zmk_custom_setting_discard(active_macro);
}
int __real_zmk_custom_settings_reset_scope(const char *subsystem, const char *key,
                                          const char *prefix, uint32_t *count) {
    return __real_zmk_custom_settings_discard_scope(subsystem, key, prefix, count);
}

/* SCOPE_LOGIC */

static int read_byte(void *arg, void *data, size_t size) {
    assert(size == 1);
    *(uint8_t *)data = *(const uint8_t *)arg;
    return 1;
}

static void reboot(void) {
    factory_seeded = 0;
    memset(factory_deleted, 0, sizeof(factory_deleted));
    assert(!factory_marker_set("v1", 1, read_byte, &stored_seed));
    assert(!factory_marker_set("deleted/Default A", 1, read_byte, &stored_deleted[0]));
    assert(!factory_marker_set("deleted/Default B", 1, read_byte, &stored_deleted[1]));
}

int main(void) {
    fail_seed = true;
    assert(runtime_macro_seed_dt_defaults() == -EIO);
    assert(installed == 2 && live[0] && live[1] && !stored_seed);
    assert(!runtime_macro_factory_note_delete("Default A"));
    assert(stored_deleted[0] && factory_deleted[0]);
    live[0] = false;
    fail_seed = false;
    reboot();
    assert(!runtime_macro_seed_dt_defaults());
    assert(installed == 3 && !live[0] && live[1] && stored_seed);
    reboot();
    assert(!runtime_macro_seed_dt_defaults() && installed == 3);
    fail_delete = true;
    assert(runtime_macro_factory_note_delete("Default B") == -EIO);
    assert(!stored_deleted[1] && !factory_deleted[1] && live[1]);
    assert(!runtime_macro_factory_note_delete("Not a default"));
    assert(factory_marker_set("v1", 0, read_byte, &stored_seed) == -EINVAL);
    assert(factory_marker_set("unknown", 1, read_byte, &stored_seed) == -EINVAL);
    struct zmk_custom_setting_state state = {0};
    struct zmk_custom_setting macro = {
        &state, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, "macro/created", &state
    };
    assert(discard_removes_referenced_macro(&macro));
    assert(__wrap_zmk_custom_setting_discard(&macro) == -EBUSY);
    assert(!discards && !releases);
    macro_referenced = false;
    edit_busy = true;
    assert(__wrap_zmk_custom_setting_discard(&macro) == -EBUSY);
    assert(!discards && !releases);
    edit_busy = false;
    assert(!__wrap_zmk_custom_setting_discard(&macro));
    assert(discards == 1 && releases == 1);
    state.flags = ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT;
    assert(!discard_removes_referenced_macro(&macro));
    assert(!__wrap_zmk_custom_setting_discard(&macro));
    assert(discards == 2 && releases == 1);
    macro_referenced = true;
    state.flags = 0;
    macro.key = "macro/missing";
    assert(!discard_removes_referenced_macro(&macro));
    macro.key = NULL;
    assert(!discard_removes_referenced_macro(&macro));
    assert(!discard_removes_referenced_macro(NULL));
    macro.key = "macro/created";
    active_macro = &macro;
    macro_live = true;
    discards = releases = 0;
    uint32_t count = 0;
    assert(__wrap_zmk_custom_settings_discard_scope(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, NULL, &count) == -EBUSY);
    assert(!discards && !releases && macro_live);
    macro_referenced = false;
    assert(!__wrap_zmk_custom_settings_discard_scope(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, "macro/other", NULL, &count));
    assert(!count && !discards && !releases && macro_live);
    state.flags = ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT;
    assert(!__wrap_zmk_custom_settings_discard_scope(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, "macro/", &count));
    assert(count == 1 && discards == 1 && !releases && macro_live);
    state.flags = 0;
    assert(!__wrap_zmk_custom_settings_discard_scope(
        ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, NULL, &count));
    assert(count == 1 && discards == 2 && releases == 1 && !macro_live);
    return 0;
}
