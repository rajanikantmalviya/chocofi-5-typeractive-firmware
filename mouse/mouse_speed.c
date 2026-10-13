#define DT_DRV_COMPAT zmk_behavior_mouse_speed

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/behavior.h>
#include <drivers/input_processor.h>
#include <zmk/behavior.h>

#include "mouse_speed_math.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static atomic_t speed_percent = ATOMIC_INIT(100);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata percentage_values[] = {
    {
        .display_name = "Percentage (100 = normal)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 1, .max = INT32_MAX},
    },
};

static const struct behavior_parameter_metadata_set percentage_set = {
    .param1_values = percentage_values,
    .param1_values_len = ARRAY_SIZE(percentage_values),
};

static const struct behavior_parameter_metadata percentage_metadata = {
    .sets_len = 1,
    .sets = &percentage_set,
};
#endif

static int select_speed(struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    if (!mouse_speed_percent_valid(binding->param1) || binding->param2 != 0) {
        LOG_ERR("Invalid mouse speed parameters: %u, %u", binding->param1, binding->param2);
        return -EINVAL;
    }
    atomic_set(&speed_percent, binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int release_speed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api mouse_speed_behavior_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = select_speed,
    .binding_released = release_speed,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &percentage_metadata,
#endif
};

#define MOUSE_SPEED_BEHAVIOR_INST(n)                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_behavior_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_SPEED_BEHAVIOR_INST)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_input_processor_mouse_speed

struct mouse_speed_data {
    uint32_t percent;
    int16_t x_remainder;
    int16_t y_remainder;
};

static int scale_movement(const struct device *dev, struct input_event *event,
                          uint32_t param1, uint32_t param2,
                          struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);
    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    struct mouse_speed_data *data = dev->data;
    uint32_t percent = atomic_get(&speed_percent);
    if (data->percent != percent) {
        data->percent = percent;
        data->x_remainder = 0;
        data->y_remainder = 0;
    }
    int16_t *remainder = event->code == INPUT_REL_X ? &data->x_remainder : &data->y_remainder;
    event->value = mouse_speed_scale(event->value, percent, remainder);
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api mouse_speed_processor_api = {
    .handle_event = scale_movement,
};

#define MOUSE_SPEED_PROCESSOR_INST(n)                                                       \
    static struct mouse_speed_data mouse_speed_data_##n = {.percent = 100};                \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &mouse_speed_data_##n, NULL, POST_KERNEL,           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_processor_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_SPEED_PROCESSOR_INST)
