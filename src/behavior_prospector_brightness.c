/*
 * behavior_prospector_brightness.c
 *
 * Runtime Prospector display brightness control behavior.
 *
 * Actions (param1 in the keymap binding):
 *   PBL_TOG (0)  Toggle the display between its current brightness and off.
 *   PBL_INC (1)  Increase brightness by `step`.
 *   PBL_DEC (2)  Decrease brightness by `step`.
 *
 * prospector_last_brightness (brightness.c) is kept in sync on every write so
 * the module's idle-timeout wake-restore lands on the keyboard-adjusted level.
 */

#define DT_DRV_COMPAT zmk_behavior_prospector_brightness

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

LOG_MODULE_REGISTER(behavior_prospector_brightness, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* The pwm-leds device and backlight child index mirror what brightness.c uses,
 * so both share the same hardware path. */
static const struct device *const pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

/* Exported by brightness.c (non-static since the idle-restore fix). */
extern uint8_t prospector_last_brightness;

/* Action codes -- must match the macros defined in custom_config.h / keymap. */
#define PBL_ACTION_TOG 0
#define PBL_ACTION_INC 1
#define PBL_ACTION_DEC 2

struct behavior_pbl_config {
    uint8_t step;
};

struct behavior_pbl_data {
    uint8_t brightness; /* 1–100, last level while screen was on */
    bool screen_on;
};

static void pbl_apply(uint8_t level)
{
    if (led_set_brightness(pwm_leds_dev, DISP_BL, level) != 0) {
        LOG_ERR("Failed to set display brightness to %d", level);
    }
}

/* Sync the module's idle-restore target so wake-from-idle lands on the
 * keyboard-adjusted level rather than the boot fixed brightness. */
static void pbl_sync(uint8_t level)
{
    prospector_last_brightness = level;
    pbl_apply(level);
}

static int pbl_init(const struct device *dev)
{
    struct behavior_pbl_data *data = dev->data;
    /* Seed from the compile-time fixed brightness (50 in ALS mode). */
#if IS_ENABLED(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS)
    data->brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;
#else
    data->brightness = 50;
#endif
    data->screen_on = true;
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_pbl_config *cfg = dev->config;
    struct behavior_pbl_data *data = dev->data;

    switch (binding->param1) {
    case PBL_ACTION_TOG:
        data->screen_on = !data->screen_on;
        pbl_apply(data->screen_on ? data->brightness : 0);
        LOG_DBG("Display toggled %s", data->screen_on ? "on" : "off");
        break;

    case PBL_ACTION_INC:
        /* Pressing INC while off wakes the display at the stored level. */
        if (!data->screen_on) {
            data->screen_on = true;
        } else {
            uint8_t next = data->brightness + cfg->step;
            data->brightness = (next > 100) ? 100 : next;
        }
        pbl_sync(data->brightness);
        LOG_DBG("Display brightness -> %d", data->brightness);
        break;

    case PBL_ACTION_DEC:
        if (!data->screen_on) {
            break; /* no-op: already off */
        }
        if (data->brightness <= cfg->step) {
            data->brightness = 1;
        } else {
            data->brightness -= cfg->step;
        }
        pbl_sync(data->brightness);
        LOG_DBG("Display brightness -> %d", data->brightness);
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_pbl_driver_api = {
    .binding_pressed  = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define PBL_INST(n)                                                              \
    static struct behavior_pbl_data behavior_pbl_data_##n = {};                  \
    static const struct behavior_pbl_config behavior_pbl_config_##n = {         \
        .step = DT_INST_PROP(n, step),                                           \
    };                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, pbl_init, NULL,                                   \
                            &behavior_pbl_data_##n,                              \
                            &behavior_pbl_config_##n,                            \
                            POST_KERNEL,                                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                 \
                            &behavior_pbl_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PBL_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
