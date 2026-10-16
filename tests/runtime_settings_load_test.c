#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

enum { ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR = 1 };
struct behavior { uint32_t behavior_id; };
struct zmk_custom_setting_value { int type; struct behavior behavior_value; };
struct state { struct behavior behavior; bool persisted; };
struct array {
    uint32_t size;
    bool has_persistent[2];
    struct zmk_custom_setting_value values[2];
    struct zmk_custom_setting_value defaults[2];
};
struct zmk_custom_setting {
    int value_type;
    const char *custom_subsystem_id;
    const char *key;
    struct state *state;
    struct array *array_state;
};
static struct state scalar_state, array_state;
static struct array array;
static struct zmk_custom_setting settings[] = {
    {1, "runtime", "scalar", &scalar_state, NULL},
    {1, "runtime", "bindings", &array_state, &array},
};
static bool ids_loaded;
static int validations, initialized_events, logged_errors, custom_settings_lock;
#define K_FOREVER 0
#define k_mutex_lock(lock, timeout) ((void)(lock), (void)(timeout))
#define k_mutex_unlock(lock) ((void)(lock))
#define ZMK_CUSTOM_SETTING_FOREACH(setting) \
    for (struct zmk_custom_setting *setting = settings; setting != settings + 2; setting++)
#define LOG_ERR(...) (logged_errors++)
struct zmk_custom_settings_initialized { int unused; };
static void raise_zmk_custom_settings_initialized(struct zmk_custom_settings_initialized event) {
    (void)event;
    initialized_events++;
}
static bool zmk_custom_setting_is_array(const struct zmk_custom_setting *setting) {
    return setting->array_state != NULL;
}
static bool setting_has_persistent_value(const struct zmk_custom_setting *setting) {
    return setting->state->persisted;
}
static int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                      const struct zmk_custom_setting_value *value) {
    (void)setting;
    validations++;
    return ids_loaded && value->behavior_value.behavior_id == 38 ? 0 : -ENODEV;
}
static bool value_equals(const struct zmk_custom_setting_value *a,
                         const struct zmk_custom_setting_value *b) {
    return a->type == b->type && a->behavior_value.behavior_id == b->behavior_value.behavior_id;
}
static int store_memory_value_locked(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value *value) {
    setting->state->behavior = value->behavior_value;
    setting->state->persisted = true;
    return 0;
}

/* LOAD_LOGIC */
/* COMMIT_LOGIC */

int main(void) {
    struct zmk_custom_setting_value saved = {1, {38}};
    assert(value_from_storage(&settings[0], &saved) == 0);
    assert(validations == 0);
    array.size = 2;
    array.values[0] = saved;
    array.has_persistent[0] = true;
    array.values[1] = (struct zmk_custom_setting_value){1, {0}};
    array.defaults[1] = array.values[1];
    array.has_persistent[1] = true;
    assert(custom_settings_handle_commit() == -ENODEV);
    assert(logged_errors == 1 && initialized_events == 0);
    ids_loaded = true;
    assert(custom_settings_handle_commit() == 0);
    assert(initialized_events == 1);
    saved.behavior_value.behavior_id = 999;
    assert(value_from_storage(&settings[0], &saved) == -ENODEV);
    assert(scalar_state.behavior.behavior_id == 38);
    assert(custom_settings_handle_commit() == 0 && initialized_events == 1);
    custom_settings_initialized = false;
    array.values[0] = saved;
    assert(custom_settings_handle_commit() == -ENODEV);
    assert(initialized_events == 1 && logged_errors == 2);
    puts("settings load ordering and immediate post-boot validation passed");
    return 0;
}
