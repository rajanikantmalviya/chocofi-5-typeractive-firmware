#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/events/position_state_changed.h>
#include <cormoran/runtime_combo/runtime_combo.h>
#include <cormoran/zmk/runtime_macro.h>
#include <cormoran/zmk/custom_settings.h>
#include "runtime_safety.h"
#include "runtime_codec.h"

K_MUTEX_DEFINE(runtime_lock);
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE),
             "Saved v0.3 behavior IDs require SETTINGS_TABLE, never implicit CRC16 migration");
static uint64_t pressed_positions;
static bool creating_macro;
static bool configuration_reading;
extern bool runtime_macro_factory_active(void);
extern struct k_msgq zmk_behavior_queue_msgq;
extern int runtime_macro_factory_note_delete(const char *name);

void runtime_safety_read_lock(void) {
    k_mutex_lock(&runtime_lock, K_FOREVER);
}

void runtime_safety_event_lock(uint32_t position, bool pressed) {
    runtime_safety_read_lock();
    if (position < 64) {
        if (pressed) {
            pressed_positions |= BIT64(position);
        } else {
            pressed_positions &= ~BIT64(position);
        }
    }
}

void runtime_safety_event_unlock(void) {
    k_mutex_unlock(&runtime_lock);
}

void runtime_safety_configuration_begin(void) {
    runtime_safety_read_lock();
    configuration_reading = true;
    runtime_safety_event_unlock();
}

void runtime_safety_configuration_end(void) {
    runtime_safety_read_lock();
    configuration_reading = false;
    runtime_safety_event_unlock();
}

int __real_zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                                 const struct zmk_behavior_binding binding,
                                 bool pressed, uint32_t wait);
int __wrap_zmk_behavior_queue_add(const struct zmk_behavior_binding_event *event,
                                 const struct zmk_behavior_binding binding,
                                 bool pressed, uint32_t wait) {
    runtime_safety_read_lock();
    int ret = __real_zmk_behavior_queue_add(event, binding, pressed, wait);
    runtime_safety_event_unlock();
    return ret;
}

#if !IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER)
int __real_raise_zmk_position_state_changed(struct zmk_position_state_changed event);
int __wrap_raise_zmk_position_state_changed(struct zmk_position_state_changed event) {
    runtime_safety_event_lock(event.position, event.state);
    int ret = __real_raise_zmk_position_state_changed(event);
    runtime_safety_event_unlock();
    return ret;
}
#endif

static bool mutation_busy(void) {
    return configuration_reading || pressed_positions || runtime_macro_factory_active() ||
           k_msgq_num_used_get(&zmk_behavior_queue_msgq);
}

bool runtime_safety_edit_allowed(void) {
    return !mutation_busy();
}

static bool macro_binding(const struct zmk_behavior_binding *binding, uint32_t slot) {
    return binding && binding->behavior_dev &&
           !strcmp(binding->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(rmacro))) &&
           binding->param1 == slot;
}

#define REFERENCE_BINDING(node, prop, idx) ZMK_KEYMAP_EXTRACT_BINDING(idx, node),
#define REFERENCE_CHILDREN(node) \
    COND_CODE_1(DT_NODE_HAS_PROP(node, bindings), \
        (DT_FOREACH_PROP_ELEM(node, bindings, REFERENCE_BINDING)), ())
static const struct zmk_behavior_binding compiled_references[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(DT_PATH(behaviors), REFERENCE_CHILDREN)
};

static int body_reference(uint32_t source_slot, const char *name, size_t size,
                          bool unsaved, void *data) {
    ARG_UNUSED(source_slot);
    ARG_UNUSED(size);
    ARG_UNUSED(unsaved);
    uint8_t body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t length;
    int ret = zmk_runtime_macro_read(name, body, sizeof(body), &length);
    if (ret) {
        return -EBUSY;
    }
    uint16_t id = zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(rmacro)));
    return runtime_body_references(body, length, id, *(uint32_t *)data) ? -EBUSY : 0;
}

static bool referenced(uint32_t slot) {
    for (uint8_t layer = 0; layer < ZMK_KEYMAP_LAYERS_LEN; layer++) {
        for (uint16_t key = 0; key < ZMK_KEYMAP_LEN; key++) {
            if (macro_binding(zmk_keymap_get_layer_binding_at_idx(layer, key), slot)) {
                return true;
            }
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(compiled_references); i++) {
        if (macro_binding(&compiled_references[i], slot)) {
            return true;
        }
    }
    for (uint32_t i = 0; i < zmk_runtime_combo_count(); i++) {
        struct zmk_runtime_combo_config combo;
        if (!zmk_runtime_combo_read(i, &combo) && combo.key_position_len &&
            macro_binding(&combo.behavior, slot)) {
            return true;
        }
    }
    return zmk_runtime_macro_for_each(body_reference, &slot) < 0;
}

/* Hold one recursive lock across multi-record high-level operations as well as
 * generic custom-setting writes. Reject edits rather than replacing active release bindings. */
#define GUARDED(name, signature, arguments) \
    int __real_##name signature; \
    int __wrap_##name signature { \
        runtime_safety_read_lock(); \
        int ret = mutation_busy() ? -EBUSY : __real_##name arguments; \
        runtime_safety_event_unlock(); \
        return ret; \
    }

GUARDED(zmk_runtime_combo_write,
        (uint32_t slot, const struct zmk_runtime_combo_config *combo, bool persist),
        (slot, combo, persist))
GUARDED(zmk_runtime_combo_delete, (uint32_t slot, bool persist), (slot, persist))
GUARDED(zmk_runtime_combo_reset, (uint32_t slot), (slot))
GUARDED(zmk_runtime_combo_write_name, (uint32_t slot, const char *name, bool persist),
        (slot, name, persist))
GUARDED(zmk_runtime_combo_write_timeout_ms, (uint16_t value, bool persist), (value, persist))
GUARDED(zmk_runtime_combo_write_slow_release, (bool value, bool persist), (value, persist))
GUARDED(zmk_runtime_combo_write_require_prior_idle_ms, (uint16_t value, bool persist),
        (value, persist))
GUARDED(zmk_runtime_macro_write,
        (const char *name, const uint8_t *data, size_t size,
         enum zmk_custom_setting_write_mode mode), (name, data, size, mode))
GUARDED(zmk_custom_setting_write_bytes,
        (const struct zmk_custom_setting *setting, const void *data, size_t size,
         enum zmk_custom_setting_write_mode mode), (setting, data, size, mode))
GUARDED(zmk_custom_setting_write_array_element,
        (const struct zmk_custom_setting *setting, const struct zmk_custom_setting_value *value,
         uint32_t size, enum zmk_custom_setting_write_mode mode), (setting, value, size, mode))
GUARDED(zmk_custom_setting_write,
        (const struct zmk_custom_setting *setting, const struct zmk_custom_setting_value *value,
         enum zmk_custom_setting_write_mode mode), (setting, value, mode))
GUARDED(zmk_custom_setting_keyspace_create,
        (struct zmk_custom_setting_keyspace *space, const char *key,
         const struct zmk_custom_setting_value *value, enum zmk_custom_setting_write_mode mode,
         const struct zmk_custom_setting **setting), (space, key, value, mode, setting))

int __real_zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *space,
                                             const char *key);
int __wrap_zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *space,
                                             const char *key) {
    runtime_safety_read_lock();
    uint32_t slot;
    int ret;
    if (mutation_busy()) {
        ret = -EBUSY;
    } else if (key && !strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX,
                               sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1) &&
               !zmk_runtime_macro_slot_for_name(key + sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1,
                                                &slot) && referenced(slot) && !creating_macro) {
        ret = -EBUSY;
    } else {
        ret = 0;
        if (!creating_macro && key &&
            !strcmp(space->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID) &&
            !strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX, sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1)) {
            ret = runtime_macro_factory_note_delete(key + sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1);
        }
        if (!ret) {
            ret = __real_zmk_custom_setting_keyspace_delete(space, key);
#if IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER)
            if (!ret && !strcmp(space->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID)) {
                extern void viewer_configuration_changed(void);
                viewer_configuration_changed();
            }
#endif
        }
    }
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_runtime_macro_create(const char *name, const uint8_t *data, size_t size,
                                   enum zmk_custom_setting_write_mode mode, uint32_t *slot);
int __wrap_zmk_runtime_macro_create(const char *name, const uint8_t *data, size_t size,
                                   enum zmk_custom_setting_write_mode mode, uint32_t *slot) {
    runtime_safety_read_lock();
    int ret = -EBUSY;
    if (!mutation_busy()) {
        creating_macro = true;
        ret = __real_zmk_runtime_macro_create(name, data, size, mode, slot);
        creating_macro = false;
    }
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_custom_setting_reset(const struct zmk_custom_setting *setting);
int __wrap_zmk_custom_setting_reset(const struct zmk_custom_setting *setting) {
    runtime_safety_read_lock();
    bool used = false;
    if (setting && setting->_keyspace &&
        !strcmp(setting->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID)) {
        const char *key = zmk_custom_setting_public_key(setting);
        uint32_t slot;
        if (key && !strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX,
                            sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1) &&
            !zmk_runtime_macro_slot_for_name(key + sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1,
                                             &slot)) {
            used = referenced(slot);
        }
    }
    int ret = mutation_busy() || used ? -EBUSY : 0;
    if (!ret && setting && setting->_keyspace &&
        !strcmp(setting->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID)) {
        const char *key = zmk_custom_setting_public_key(setting);
        if (!key || strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX,
                            sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1)) {
            ret = -EINVAL;
        } else {
            ret = runtime_macro_factory_note_delete(key + sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1);
        }
    }
    if (!ret) {
        ret = __real_zmk_custom_setting_reset(setting);
    }
    runtime_safety_event_unlock();
    return ret;
}

static bool discard_removes_referenced_macro(const struct zmk_custom_setting *setting) {
    if (!setting || !setting->_keyspace ||
        strcmp(setting->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID) ||
        (setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT)) {
        return false;
    }
    const char *key = zmk_custom_setting_public_key(setting);
    uint32_t slot;
    return key && !strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX,
                          sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1) &&
           !zmk_runtime_macro_slot_for_name(key + sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1,
                                            &slot) && referenced(slot);
}

int __real_zmk_custom_setting_discard(const struct zmk_custom_setting *setting);
extern int runtime_keyspace_discard_created(const struct zmk_custom_setting *setting);
int __wrap_zmk_custom_setting_discard(const struct zmk_custom_setting *setting) {
    runtime_safety_read_lock();
    int ret = mutation_busy() || discard_removes_referenced_macro(setting) ? -EBUSY :
              __real_zmk_custom_setting_discard(setting);
    if (!ret && setting && setting->_keyspace &&
        !strcmp(setting->custom_subsystem_id, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID) &&
        !(setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT)) {
        /* Upstream discard leaves a never-saved keyspace entry unchanged. */
        ret = runtime_keyspace_discard_created(setting);
    }
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_runtime_macro_delete(const char *name);
int __wrap_zmk_runtime_macro_delete(const char *name) {
    runtime_safety_read_lock();
    uint32_t slot;
    int ret = zmk_runtime_macro_slot_for_name(name, &slot);
    if (!ret) {
        ret = mutation_busy() || referenced(slot) ? -EBUSY : __real_zmk_runtime_macro_delete(name);
    }
    runtime_safety_event_unlock();
    return ret;
}

struct reset_scope {
    const char *key;
    const char *prefix;
    bool discard;
};

static bool macro_in_scope(const char *name, const struct reset_scope *scope,
                           char *key, size_t capacity) {
    snprintk(key, capacity, "%s%s", ZMK_RUNTIME_MACRO_KEY_PREFIX, name);
    return (!scope->key || !scope->key[0] || !strcmp(scope->key, key)) &&
           (!scope->prefix || !scope->prefix[0] ||
            !strncmp(scope->prefix, key, strlen(scope->prefix)));
}

static int referenced_in_scope(uint32_t slot, const char *name, size_t size,
                                bool unsaved, void *data) {
    ARG_UNUSED(size);
    ARG_UNUSED(unsaved);
    struct reset_scope *scope = data;
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    if (!macro_in_scope(name, scope, key, sizeof(key))) {
        return 0;
    }
    if (scope->discard) {
        const struct zmk_custom_setting *setting =
            zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, key);
        return discard_removes_referenced_macro(setting) ? -EBUSY : 0;
    }
    return referenced(slot) ? -EBUSY : runtime_macro_factory_note_delete(name);
}

static int discard_created_in_scope(uint32_t slot, const char *name, size_t size,
                                    bool unsaved, void *data) {
    ARG_UNUSED(slot);
    ARG_UNUSED(size);
    ARG_UNUSED(unsaved);
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    if (!macro_in_scope(name, data, key, sizeof(key))) {
        return 0;
    }
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, key);
    return setting && !(setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT) ?
           runtime_keyspace_discard_created(setting) : 0;
}

int __real_zmk_custom_settings_reset_scope(const char *subsystem, const char *key,
                                           const char *prefix, uint32_t *count);
int __wrap_zmk_custom_settings_reset_scope(const char *subsystem, const char *key,
                                           const char *prefix, uint32_t *count) {
    runtime_safety_read_lock();
    struct reset_scope scope = {.key = key, .prefix = prefix};
    int ret = mutation_busy() ? -EBUSY : 0;
    if (!ret && (!subsystem || !subsystem[0] ||
                 !strcmp(subsystem, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID))) {
        ret = zmk_runtime_macro_for_each(referenced_in_scope, &scope);
    }
    if (!ret) {
        ret = __real_zmk_custom_settings_reset_scope(subsystem, key, prefix, count);
    }
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_custom_settings_discard_scope(const char *subsystem, const char *key,
                                             const char *prefix, uint32_t *count);
int __wrap_zmk_custom_settings_discard_scope(const char *subsystem, const char *key,
                                             const char *prefix, uint32_t *count) {
    runtime_safety_read_lock();
    struct reset_scope scope = {.key = key, .prefix = prefix, .discard = true};
    int ret = mutation_busy() ? -EBUSY : 0;
    if (!ret && (!subsystem || !subsystem[0] ||
                 !strcmp(subsystem, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID))) {
        ret = zmk_runtime_macro_for_each(referenced_in_scope, &scope);
    }
    if (!ret) {
        ret = __real_zmk_custom_settings_discard_scope(subsystem, key, prefix, count);
    }
    if (!ret && (!subsystem || !subsystem[0] ||
                 !strcmp(subsystem, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID))) {
        /* The upstream scope calls its own discard directly, bypassing --wrap. */
        ret = zmk_runtime_macro_for_each(discard_created_in_scope, &scope);
    }
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_runtime_macro_rename(const char *old_name, const char *new_name,
                                   enum zmk_custom_setting_write_mode mode);
int __wrap_zmk_runtime_macro_rename(const char *old_name, const char *new_name,
                                   enum zmk_custom_setting_write_mode mode) {
    runtime_safety_read_lock();
    uint32_t slot;
    int ret = zmk_runtime_macro_slot_for_name(old_name, &slot);
    if (!ret) {
        ret = mutation_busy() ? -EBUSY :
              __real_zmk_runtime_macro_rename(old_name, new_name, mode);
    }
    runtime_safety_event_unlock();
    return ret;
}
