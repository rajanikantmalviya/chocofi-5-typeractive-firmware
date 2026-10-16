#include <stdarg.h>
#include <errno.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/physical_layouts.h>
#include <zmk/studio/core.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <cormoran/runtime_combo/runtime_combo.h>
#include <cormoran/zmk/runtime_macro.h>
#include <cormoran/zmk/custom_settings.h>
#include "viewer_ble.h"
#include "runtime_safety.h"

LOG_MODULE_REGISTER(keyboard_viewer, LOG_LEVEL_WRN);

#define MAX_KEYS 64
BUILD_ASSERT(ZMK_KEYMAP_LEN <= MAX_KEYS, "Viewer supports up to 64 physical positions");

enum kind { OTHER, KEY, HOLD_TAP, MOMENTARY_LAYER, TOGGLE_LAYER, TO_LAYER, STICKY,
            TRANSPARENT, NONE, TAP_DANCE, MOUSE, NATIVE_MACRO, RUNTIME_MACRO };
struct behavior_description {
    const char *device;
    const char *name;
    enum kind kind;
    const struct zmk_behavior_binding *children;
    size_t children_len;
};

#define CHILD_BINDING(node, prop, idx) ZMK_KEYMAP_EXTRACT_BINDING(idx, node),
#define CHILDREN(node)                                                                      \
    COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                            \
        (static const struct zmk_behavior_binding DT_CAT(node, _viewer_children)[] = {     \
            DT_FOREACH_PROP_ELEM(node, bindings, CHILD_BINDING)};), ())
DT_FOREACH_CHILD_STATUS_OKAY(DT_PATH(behaviors), CHILDREN)

#define KIND(node)                                                                         \
    (DT_NODE_HAS_COMPAT(node, zmk_behavior_key_press) ? KEY :                                \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_hold_tap) ? HOLD_TAP :                             \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_momentary_layer) ? MOMENTARY_LAYER :               \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_toggle_layer) ? TOGGLE_LAYER :                     \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_to_layer) ? TO_LAYER :                             \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_sticky_key) ? STICKY :                             \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_transparent) ? TRANSPARENT :                       \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_none) ? NONE :                                     \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_tap_dance) ? TAP_DANCE :                           \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_mouse_key_press) ? MOUSE :                         \
     DT_NODE_HAS_COMPAT(node, zmk_behavior_macro) ? NATIVE_MACRO :                           \
     DT_NODE_HAS_COMPAT(node, cormoran_zmk_behavior_runtime_macro) ? RUNTIME_MACRO : OTHER)
#define DESCRIPTION(node)                                                                  \
    {.device = DEVICE_DT_NAME(node),                                                        \
     .name = DT_PROP_OR(node, display_name, DEVICE_DT_NAME(node)), .kind = KIND(node),       \
     COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                           \
        (.children = DT_CAT(node, _viewer_children),                                        \
         .children_len = DT_PROP_LEN(node, bindings)),                                      \
        (.children = NULL, .children_len = 0))},
static const struct behavior_description descriptions[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(DT_PATH(behaviors), DESCRIPTION)
};
static const char *const kind_names[] = {
    "other", "key", "hold-tap", "momentary-layer", "toggle-layer", "to-layer",
    "sticky", "transparent", "none", "tap-dance", "mouse", "macro", "runtime-macro"
};

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(keyboard_viewer_uart));
RING_BUF_DECLARE(tx, 2048);
K_SEM_DEFINE(tx_space, 0, 1);
struct viewer_command { uint8_t command; bool ble; };
K_MSGQ_DEFINE(requests, sizeof(struct viewer_command), 4, 1);
K_SEM_DEFINE(viewer_wake, 0, 1);
static atomic_t dirty;
static atomic_t config_revision = ATOMIC_INIT(1);
static bool ble_sending;
static struct k_spinlock tx_lock;
static struct k_spinlock state_lock;
static uint64_t pressed_bits;
static uint64_t released_bits;
static uint64_t ble_released_bits;
static uint8_t origins[MAX_KEYS];
static uint32_t sequence;
static bool available;
static bool sending;

int viewer_request(uint8_t command, bool ble) {
    struct viewer_command request = {.command = command, .ble = ble};
    int ret = k_msgq_put(&requests, &request, K_NO_WAIT);
    if (!ret) {
        k_sem_give(&viewer_wake);
    }
    return ret;
}

void viewer_changed(void) {
    atomic_set(&dirty, 1);
    k_sem_give(&viewer_wake);
}

static bool transparent(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(descriptions); i++) {
        if (descriptions[i].kind == TRANSPARENT && !strcmp(descriptions[i].device, name)) {
            return true;
        }
    }
    return false;
}

static uint8_t origin_layer(uint32_t position) {
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        uint8_t id = zmk_keymap_layer_index_to_id(index);
        if (id == UINT8_MAX || !zmk_keymap_layer_active(id)) {
            continue;
        }
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(id, position);
        if (!binding || !binding->behavior_dev || !transparent(binding->behavior_dev)) {
            return id;
        }
    }
    return zmk_keymap_layer_default();
}

int __real_raise_zmk_position_state_changed(struct zmk_position_state_changed event);

int __wrap_raise_zmk_position_state_changed(struct zmk_position_state_changed event) {
    runtime_safety_event_lock(event.position, event.state);
    if (event.position < MAX_KEYS) {
        uint8_t origin = event.state ? origin_layer(event.position) : 0;
        k_spinlock_key_t key = k_spin_lock(&state_lock);
        uint64_t bit = BIT64(event.position);
        if (event.state) {
            origins[event.position] = origin;
            pressed_bits |= bit;
        } else {
            pressed_bits &= ~bit;
            released_bits |= bit;
            ble_released_bits |= bit;
        }
        sequence++;
        k_spin_unlock(&state_lock, key);
        viewer_changed();
    }
    // Observe before combo/hold-tap capture; always pass the original event unchanged.
    int ret = __real_raise_zmk_position_state_changed(event);
    runtime_safety_event_unlock();
    return ret;
}

int __real_zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event, bool pressed);

int __wrap_zmk_behavior_invoke_binding(const struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event, bool pressed) {
    if (pressed && event.position < MAX_KEYS && event.layer >= 0 &&
        event.layer < ZMK_KEYMAP_LAYERS_LEN && binding->behavior_dev &&
        !transparent(binding->behavior_dev)) {
        // A captured key may resolve on a different layer after a pending hold-tap.
        k_spinlock_key_t key = k_spin_lock(&state_lock);
        bool changed = (pressed_bits & BIT64(event.position)) &&
                       origins[event.position] != event.layer;
        if (changed) {
            origins[event.position] = event.layer;
            sequence++;
        }
        k_spin_unlock(&state_lock, key);
        if (changed) {
            viewer_changed();
        }
    }
    return __real_zmk_behavior_invoke_binding(binding, event, pressed);
}

static void uart_irq(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev)) {
            uint8_t bytes[16];
            int count = uart_fifo_read(dev, bytes, sizeof(bytes));
            for (int i = 0; i < count; i++) {
                if (bytes[i] == 'C' || bytes[i] == 'S') {
                    if (viewer_request(bytes[i], false) != 0) {
                        LOG_WRN("Viewer request queue full");
                    }
                }
            }
        }
        if (uart_irq_tx_ready(dev)) {
            uint8_t *data;
            k_spinlock_key_t key = k_spin_lock(&tx_lock);
            uint32_t size = ring_buf_get_claim(&tx, &data, 64);
            int sent = size ? uart_fifo_fill(dev, data, size) : 0;
            ring_buf_get_finish(&tx, sent > 0 ? sent : 0);
            if (ring_buf_is_empty(&tx)) {
                uart_irq_tx_disable(dev);
            }
            k_spin_unlock(&tx_lock, key);
            k_sem_give(&tx_space);
        }
    }
}

static void send_bytes(const char *data, size_t len) {
#if IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER_BLE)
    if (ble_sending) {
        if (sending && viewer_ble_send(data, len)) {
            sending = false;
        }
        return;
    }
#endif
    int64_t deadline = k_uptime_get() + 250;
    while (sending && len) {
        uint32_t dtr = 0;
        if (uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) || !dtr ||
            k_uptime_get() >= deadline) {
            sending = false;
            LOG_WRN("Viewer disconnected or transmit timeout");
            return;
        }
        k_spinlock_key_t key = k_spin_lock(&tx_lock);
        uint32_t written = ring_buf_put(&tx, (const uint8_t *)data, len);
        uart_irq_tx_enable(uart);
        k_spin_unlock(&tx_lock, key);
        data += written;
        len -= written;
        if (len) {
            k_sem_take(&tx_space, K_MSEC(10));
        }
    }
}

static void output(const char *format, ...) {
    char buffer[160];
    va_list args;
    va_start(args, format);
    int len = vsnprintk(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len < 0 || len >= sizeof(buffer)) {
        LOG_ERR("Viewer output chunk overflow");
        sending = false;
        return;
    }
    send_bytes(buffer, len);
}

static void string(const char *value) {
    output("\"");
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            output("\\%c", *p);
        } else if (*p < 32) {
            output("\\u%04x", *p);
        } else {
            send_bytes((const char *)p, 1);
        }
    }
    output("\"");
}

static void binding(const struct zmk_behavior_binding *value) {
    uint16_t id = value && value->behavior_dev ?
        zmk_behavior_get_local_id(value->behavior_dev) : 0;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    if (value && !value->behavior_dev && value->local_id) {
        id = value->local_id;
    }
#endif
    output("[%u,%u,%u]", id,
        value ? value->param1 : 0, value ? value->param2 : 0);
}

#define POSITIONS(node)                                                                    \
    static const uint32_t DT_CAT(node, _viewer_positions)[] = DT_PROP(node, key_positions); \
    static const uint32_t DT_CAT(node, _viewer_layers)[] =                                  \
        COND_CODE_1(DT_NODE_HAS_PROP(node, layers), (DT_PROP(node, layers)), ({0}));
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_combos)
DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), POSITIONS)
#endif

#define COMBO(node) do {                                                                   \
    output("{\"type\":\"combo\",\"name\":");                                                \
    string(DT_NODE_FULL_NAME(node));                                                       \
    output(",\"positions\":[");                                                            \
    for (size_t i = 0; i < DT_PROP_LEN(node, key_positions); i++) {                         \
        output("%s%u", i ? "," : "", DT_CAT(node, _viewer_positions)[i]);                   \
    }                                                                                     \
    output("],\"layers\":[");                                                              \
    for (size_t i = 0; i < DT_PROP_LEN_OR(node, layers, 0); i++) {                          \
        output("%s%u", i ? "," : "", DT_CAT(node, _viewer_layers)[i]);                      \
    }                                                                                     \
    const struct zmk_behavior_binding value = ZMK_KEYMAP_EXTRACT_BINDING(0, node);          \
    output("],\"binding\":");                                                              \
    binding(&value);                                                                      \
    output(",\"timeoutMs\":%u}\n", DT_PROP(node, timeout_ms));                              \
} while (0);

static uint32_t macro_uvar(const uint8_t *data, size_t size, size_t *offset) {
    uint32_t value = 0;
    for (uint32_t shift = 0; shift < 32 && *offset < size; shift += 7) {
        uint8_t byte = data[(*offset)++];
        value |= (uint32_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return value;
        }
    }
    *offset = size;
    return 0;
}

static int viewer_macro(uint32_t slot, const char *name, size_t encoded_size,
                        bool unsaved, void *user_data) {
    ARG_UNUSED(encoded_size);
    ARG_UNUSED(user_data);
    uint8_t data[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
    size_t size = 0;
    runtime_safety_read_lock();
    int ret = zmk_runtime_macro_read(name, data, sizeof(data), &size);
    runtime_safety_event_unlock();
    if (ret || zmk_runtime_macro_validate_encoded(data, size)) {
        return 0;
    }
    output("{\"type\":\"macro\",\"slot\":%u,\"name\":", slot);
    string(name);
    output(",\"persisted\":%s,\"hasUnsavedChanges\":%s,\"steps\":[",
           unsaved ? "false" : "true", unsaved ? "true" : "false");
    size_t offset = size ? 1 : 0;
    bool first = true;
    while (offset < size) {
        uint8_t opcode = data[offset++];
        if (opcode == ZMK_RUNTIME_MACRO_OP_KEY_TAP_SEQUENCE) {
            uint32_t count = macro_uvar(data, size, &offset);
            for (uint32_t i = 0; i < count && offset < size; i++) {
                uint32_t keycode;
                if (!zmk_runtime_macro_unpack_key_tap(data[offset++], &keycode)) {
                    output("%s{\"kind\":\"tap\",\"binding\":[%u,%u,0]}",
                           first ? "" : ",",
                           zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp))), keycode);
                    first = false;
                }
            }
        } else if (opcode == ZMK_RUNTIME_MACRO_OP_DELAY) {
            uint32_t delay = macro_uvar(data, size, &offset);
            output("%s{\"kind\":\"delay\",\"delayMs\":%u}", first ? "" : ",", delay);
            first = false;
        } else {
            uint32_t id = macro_uvar(data, size, &offset);
            uint32_t p1 = macro_uvar(data, size, &offset);
            uint32_t p2 = macro_uvar(data, size, &offset);
            output("%s{\"kind\":\"%s\",\"binding\":[%u,%u,%u]}",
                   first ? "" : ",", opcode == ZMK_RUNTIME_MACRO_OP_DOWN ? "press" :
                   opcode == ZMK_RUNTIME_MACRO_OP_UP ? "release" : "tap", id, p1, p2);
            first = false;
        }
    }
    output("]}\n");
    return 0;
}

static void native_macro(const struct behavior_description *desc) {
    output("{\"type\":\"macro\",\"behaviorId\":%u,\"source\":\"native\",\"name\":",
           zmk_behavior_get_local_id(desc->device));
    string(desc->name);
    output(",\"steps\":[");
    const char *mode = "tap";
    bool first = true;
    for (size_t i = 0; i < desc->children_len; i++) {
        const struct zmk_behavior_binding *step = &desc->children[i];
        if (!strcmp(step->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(macro_press)))) {
            mode = "press";
        } else if (!strcmp(step->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(macro_release)))) {
            mode = "release";
        } else if (!strcmp(step->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(macro_tap)))) {
            mode = "tap";
        } else if (!strcmp(step->behavior_dev,
                           DEVICE_DT_NAME(DT_NODELABEL(macro_pause_for_release)))) {
            output("%s{\"kind\":\"pause-for-release\"}", first ? "" : ",");
            first = false;
        } else if (!strcmp(step->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(macro_wait_time)))) {
            output("%s{\"kind\":\"delay\",\"delayMs\":%u}", first ? "" : ",", step->param1);
            first = false;
        } else {
            output("%s{\"kind\":\"%s\",\"binding\":", first ? "" : ",", mode);
            binding(step);
            output("}");
            first = false;
        }
    }
    output("]}\n");
}

static void configuration(void) {
    struct zmk_physical_layout const *const *layouts;
    size_t count = zmk_physical_layouts_get_list(&layouts);
    int selected = zmk_physical_layouts_get_selected();
    if (selected < 0 || selected >= count) {
        output("{\"type\":\"error\",\"message\":\"No physical layout selected\"}\n");
        return;
    }
    const struct zmk_physical_layout *layout = layouts[selected];
    // Freeze definitions, not HID processing; never hold a mutex across transport writes.
    runtime_safety_configuration_begin();
    output("{\"type\":\"hello\",\"version\":1,\"deviceName\":");
    string(CONFIG_ZMK_KEYBOARD_NAME);
    output(",\"layoutName\":");
    string(layout->display_name);
    output(",\"defaultLayer\":%u}\n", zmk_keymap_layer_default());
    output("{\"type\":\"capabilities\",\"runtimeCombos\":true,\"runtimeMacros\":true,");
    output("\"bleStreaming\":%s,", IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER_BLE) ? "true" : "false");
    output("\"behaviorIdPolicy\":\"settings-table\",\"macroSlotsStableOnRename\":true,");
    output("\"comboSlots\":%u,\"comboMaxKeys\":%u,\"macroSlots\":%u,",
           CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO,
           CONFIG_ZMK_RUNTIME_MACRO_COUNT);
    output("\"comboNameMaxBytes\":63,");
    output("\"macroMaxBytes\":%u,\"macroPoolBytes\":%u,\"macroNameMaxBytes\":%u,",
           CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES, CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES,
           CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN);
    output("\"macroPauseForRelease\":false,\"configRevision\":%u}\n",
           (uint32_t)atomic_get(&config_revision));
    for (size_t i = 0; i < layout->keys_len; i++) {
        const struct zmk_key_physical_attrs *key = &layout->keys[i];
        output("{\"type\":\"key\",\"position\":%u,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
               i, key->x, key->y, key->width, key->height);
#if IS_ENABLED(CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION)
        output(",\"r\":%d,\"rx\":%d,\"ry\":%d", key->r, key->rx, key->ry);
#else
        output(",\"r\":0,\"rx\":%d,\"ry\":%d", key->x, key->y);
#endif
        output("}\n");
    }
    for (size_t i = 0; i < ARRAY_SIZE(descriptions); i++) {
        const struct behavior_description *desc = &descriptions[i];
        uint16_t id = zmk_behavior_get_local_id(desc->device);
        if (id == UINT16_MAX) {
            continue;
        }
        output("{\"type\":\"behavior\",\"id\":%u,\"name\":", id);
        string(desc->name);
        output(",\"kind\":");
        string(kind_names[desc->kind]);
        output(",\"children\":[");
        size_t children_len = desc->kind == HOLD_TAP || desc->kind == STICKY ||
            desc->kind == TAP_DANCE ? desc->children_len : 0;
        for (size_t b = 0; b < children_len; b++) {
            output("%s", b ? "," : "");
            binding(&desc->children[b]);
        }
        output("]}\n");
        if (desc->kind == NATIVE_MACRO) {
            native_macro(desc);
        }
    }
    for (uint8_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        uint8_t id = zmk_keymap_layer_index_to_id(index);
        if (id == UINT8_MAX) {
            break;
        }
        output("{\"type\":\"layer\",\"id\":%u,\"name\":", id);
        string(zmk_keymap_layer_name(id));
        output(",\"bindings\":[");
        for (size_t p = 0; p < layout->keys_len; p++) {
            output("%s", p ? "," : "");
            binding(zmk_keymap_get_layer_binding_at_idx(id, p));
        }
        output("]}\n");
    }
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_combos)
    DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), COMBO)
#endif
    for (uint32_t slot = 0; slot < zmk_runtime_combo_count(); slot++) {
        struct zmk_runtime_combo_config combo;
        char name[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
        runtime_safety_read_lock();
        int ret = zmk_runtime_combo_read(slot, &combo);
        zmk_runtime_combo_read_name(slot, name, sizeof(name));
        runtime_safety_event_unlock();
        if (ret || !combo.key_position_len) {
            continue;
        }
        output("{\"type\":\"combo\",\"slot\":%u,\"enabled\":%s,\"name\":",
               slot, combo.enabled ? "true" : "false");
        string(name);
        output(",\"positions\":[");
        for (size_t i = 0; i < combo.key_position_len; i++) {
            output("%s%d", i ? "," : "", combo.key_positions[i]);
        }
        output("],\"layers\":[");
        bool first = true;
        for (uint32_t layer = 0; layer < 32; layer++) {
            if (combo.layer_mask & BIT(layer)) {
                output("%s%u", first ? "" : ",", layer);
                first = false;
            }
        }
        output("],\"binding\":");
        binding(&combo.behavior);
        output(",\"timeoutMs\":%u,\"source\":\"%s\"}\n",
               combo.timeout_ms, zmk_runtime_combo_has_override(slot) ? "runtime" : "default");
    }
    zmk_runtime_macro_for_each(viewer_macro, NULL);
    output("{\"type\":\"ready\"}\n");
    runtime_safety_configuration_end();
}

static void snapshot(void) {
    uint8_t saved_origins[MAX_KEYS];
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    uint64_t pressed = pressed_bits;
    uint64_t released = ble_sending ? ble_released_bits : released_bits;
    if (ble_sending) {
        ble_released_bits = 0;
    } else {
        released_bits = 0;
    }
    uint32_t seq = sequence;
    memcpy(saved_origins, origins, sizeof(origins));
    k_spin_unlock(&state_lock, key);
    output("{\"type\":\"state\",\"sequence\":%u,\"configRevision\":%u,\"pressed\":[",
           seq, (uint32_t)atomic_get(&config_revision));
    bool first = true;
    for (size_t p = 0; p < MAX_KEYS; p++) {
        if (pressed & BIT64(p)) {
            output("%s[%u,%u]", first ? "" : ",", p, saved_origins[p]);
            first = false;
        }
    }
    output("],\"released\":[");
    first = true;
    for (size_t p = 0; p < MAX_KEYS; p++) {
        if (released & BIT64(p)) {
            output("%s%u", first ? "" : ",", p);
            first = false;
        }
    }
    output("],\"layers\":[");
    first = true;
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        uint8_t id = zmk_keymap_layer_index_to_id(index);
        if (id != UINT8_MAX && zmk_keymap_layer_active(id)) {
            output("%s%u", first ? "" : ",", id);
            first = false;
        }
    }
    output("]}\n");
}

static void worker(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    struct viewer_command request;
    int64_t last_stream = 0;
    while (true) {
        k_sem_take(&viewer_wake, K_FOREVER);
        int ret = k_msgq_get(&requests, &request, K_NO_WAIT);
#if IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER_BLE)
        if (ret && viewer_ble_streaming() && atomic_get(&dirty)) {
            int64_t remaining = 34 - (k_uptime_get() - last_stream);
            if (remaining > 0) {
                k_sleep(K_MSEC(remaining));
            }
            request = (struct viewer_command){.command = 'S', .ble = true};
            atomic_clear(&dirty);
            last_stream = k_uptime_get();
            ret = 0;
        }
#endif
        if (k_msgq_num_used_get(&requests)) {
            k_sem_give(&viewer_wake);
        }
        if (ret) {
            continue;
        }
        ble_sending = request.ble;
        if (!ble_sending) {
            uint32_t dtr = 0;
            if (!available || uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) || !dtr) {
                continue;
            }
        }
        if (request.command == 'C' && !ble_sending) {
            k_spinlock_key_t key = k_spin_lock(&tx_lock);
            ring_buf_reset(&tx);
            k_spin_unlock(&tx_lock, key);
        }
        if (request.command == 'C') {
            k_spinlock_key_t key = k_spin_lock(&state_lock);
            if (ble_sending) {
                ble_released_bits = 0;
            } else {
                released_bits = 0;
            }
            k_spin_unlock(&state_lock, key);
        }
        sending = true;
        if (zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
            output("{\"type\":\"error\",\"message\":\"Unlock the keyboard in Studio first\"}\n");
        } else if (request.command == 'C') {
            configuration();
        } else {
            snapshot();
        }
#if IS_ENABLED(CONFIG_ZMK_KEYBOARD_VIEWER_BLE)
        if (ble_sending) {
            viewer_ble_flush();
        }
#endif
    }
}

K_THREAD_DEFINE(viewer_thread, 2048, worker, NULL, NULL, NULL, 10, 0, 0);

static int viewer_init(void) {
    if (!device_is_ready(uart)) {
        LOG_ERR("Viewer UART unavailable; normal keyboard input is unaffected");
        return 0;
    }
    int err = uart_irq_callback_user_data_set(uart, uart_irq, NULL);
    if (err) {
        LOG_ERR("Viewer UART initialization failed: %d", err);
        return 0;
    }
    available = true;
    uart_irq_rx_enable(uart);
    return 0;
}

SYS_INIT(viewer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void viewer_configuration_changed(void) {
    atomic_inc(&config_revision);
    viewer_changed();
}

static int viewer_events(const zmk_event_t *event) {
    if (as_zmk_custom_setting_changed(event)) {
        atomic_inc(&config_revision);
        zmk_runtime_combo_invalidate_cache();
    }
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    sequence++;
    k_spin_unlock(&state_lock, key);
    viewer_changed();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(viewer_updates, viewer_events);
ZMK_SUBSCRIPTION(viewer_updates, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(viewer_updates, zmk_custom_setting_changed);

int __real_zmk_keymap_set_layer_binding_at_idx(zmk_keymap_layer_id_t layer, uint16_t idx,
                                              struct zmk_behavior_binding binding);
int __wrap_zmk_keymap_set_layer_binding_at_idx(zmk_keymap_layer_id_t layer, uint16_t idx,
                                              struct zmk_behavior_binding binding) {
    runtime_safety_read_lock();
    int ret = runtime_safety_edit_allowed() ?
        __real_zmk_keymap_set_layer_binding_at_idx(layer, idx, binding) : -EBUSY;
    runtime_safety_event_unlock();
    if (!ret) {
        atomic_inc(&config_revision);
        viewer_changed();
    }
    return ret;
}

#define KEYMAP_DEFINITION(name, signature, arguments) \
    int __real_##name signature; \
    int __wrap_##name signature { \
        runtime_safety_read_lock(); \
        int ret = runtime_safety_edit_allowed() ? __real_##name arguments : -EBUSY; \
        runtime_safety_event_unlock(); \
        if (ret >= 0) { \
            atomic_inc(&config_revision); \
            viewer_changed(); \
        } \
        return ret; \
    }

KEYMAP_DEFINITION(zmk_keymap_discard_changes, (void), ())
KEYMAP_DEFINITION(zmk_keymap_reset_settings, (void), ())
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_LAYER_REORDERING)
KEYMAP_DEFINITION(zmk_keymap_set_layer_name,
                  (zmk_keymap_layer_id_t id, const char *name, size_t size), (id, name, size))
KEYMAP_DEFINITION(zmk_keymap_add_layer, (void), ())
KEYMAP_DEFINITION(zmk_keymap_remove_layer, (zmk_keymap_layer_index_t index), (index))
KEYMAP_DEFINITION(zmk_keymap_restore_layer,
                  (zmk_keymap_layer_id_t id, zmk_keymap_layer_index_t index), (id, index))
KEYMAP_DEFINITION(zmk_keymap_move_layer,
                  (zmk_keymap_layer_index_t start, zmk_keymap_layer_index_t end), (start, end))
#endif
