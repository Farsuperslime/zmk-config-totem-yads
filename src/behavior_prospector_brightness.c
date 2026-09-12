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
 * Hold-to-repeat: holding INC or DEC steps once immediately, waits
 * PBL_REPEAT_DELAY_MS (so a deliberate tap never overshoots), then keeps
 * stepping every PBL_REPEAT_INTERVAL_MS until the key is released. A delayed
 * work item drives the repeat, so nothing blocks the keymap/event thread.
 *
 * This driver is compiled into every build, because the behavior node it backs
 * lives in the shared totem.keymap. The display backlight only exists on
 * builds that include the prospector_adapter shield, so the hardware access is
 * guarded on CONFIG_SHIELD_PROSPECTOR_ADAPTER (a Kconfig symbol, hence valid
 * here even though it is not visible to the devicetree preprocessor). Without
 * it the behavior accepts presses and does nothing.
 *
 * On prospector builds, prospector_last_brightness (brightness.c) is kept in
 * sync on every write so the module's idle-timeout wake-restore lands on the
 * keyboard-adjusted level.
 */

#define DT_DRV_COMPAT zmk_behavior_prospector_brightness

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

LOG_MODULE_REGISTER(behavior_prospector_brightness, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_SHIELD_PROSPECTOR_ADAPTER)

/* The pwm-leds device and backlight child index mirror what brightness.c uses,
 * so both share the same hardware path. */
static const struct device *const pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

/* Exported by brightness.c (non-static since the idle-restore fix). */
extern uint8_t prospector_last_brightness;

#endif /* CONFIG_SHIELD_PROSPECTOR_ADAPTER */

/* Action codes -- must match the macros defined in custom_config.h / keymap. */
#define PBL_ACTION_TOG 0
#define PBL_ACTION_INC 1
#define PBL_ACTION_DEC 2
/* Sentinel for "no repeat armed"; deliberately not a valid action code. */
#define PBL_ACTION_NONE 0xFF

/* Wait this long after the initial step before auto-repeat kicks in, so a
 * deliberate tap registers as exactly one step. Then step again every
 * PBL_REPEAT_INTERVAL_MS while the key stays held. */
#define PBL_REPEAT_DELAY_MS 400
#define PBL_REPEAT_INTERVAL_MS 120

struct behavior_pbl_config {
    uint8_t step;
};

struct behavior_pbl_data {
    uint8_t brightness; /* 1-100, last level while screen was on */
    uint8_t step;       /* copied from DT config; used by the repeat handler */
    bool screen_on;
    struct k_work_delayable repeat;
    uint8_t repeat_action; /* PBL_ACTION_INC/DEC while held, else PBL_ACTION_NONE */
};

static void pbl_apply(uint8_t level)
{
#if IS_ENABLED(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
    if (led_set_brightness(pwm_leds_dev, DISP_BL, level) != 0) {
        LOG_ERR("Failed to set display brightness to %d", level);
    }
#else
    ARG_UNUSED(level);
#endif
}

/* Sync the module's idle-restore target so wake-from-idle lands on the
 * keyboard-adjusted level rather than the boot fixed brightness. */
static void pbl_sync(uint8_t level)
{
#if IS_ENABLED(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
    prospector_last_brightness = level;
#endif
    pbl_apply(level);
}

/* Applies one brightness step. Returns true if the level actually changed, so
 * the caller knows whether auto-repeat is worth arming. */
static bool pbl_step(struct behavior_pbl_data *data, bool up)
{
    if (!data->screen_on) {
        if (!up) {
            return false; /* DEC while toggled off: no-op, as before */
        }
        /* Pressing INC while off wakes the display at the stored level. */
        data->screen_on = true;
    } else if (up) {
        uint8_t next = data->brightness + data->step;
        data->brightness = (next > 100) ? 100 : next;
    } else if (data->brightness <= data->step) {
        data->brightness = 1;
    } else {
        data->brightness -= data->step;
    }

    pbl_sync(data->brightness);
    LOG_DBG("Display brightness -> %d", data->brightness);
    return true;
}

static void pbl_repeat_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct behavior_pbl_data *data = CONTAINER_OF(dwork, struct behavior_pbl_data, repeat);

    if (data->repeat_action == PBL_ACTION_NONE) {
        return;
    }

    bool up = (data->repeat_action == PBL_ACTION_INC);
    if (!pbl_step(data, up)) {
        /* Nothing left to do (e.g. DEC while toggled off): stop repeating. */
        data->repeat_action = PBL_ACTION_NONE;
        return;
    }

    k_work_reschedule(&data->repeat, K_MSEC(PBL_REPEAT_INTERVAL_MS));
}

static int pbl_init(const struct device *dev)
{
    struct behavior_pbl_data *data = dev->data;
    const struct behavior_pbl_config *cfg = dev->config;

    k_work_init_delayable(&data->repeat, pbl_repeat_handler);
    data->repeat_action = PBL_ACTION_NONE;
    data->step = cfg->step;

    /* Seed from the compile-time fixed brightness. CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
     * is an int (range 1-100) that only exists without the ambient-light sensor,
     * so test it with #ifdef, not IS_ENABLED(). */
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
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
    struct behavior_pbl_data *data = dev->data;

    switch (binding->param1) {
    case PBL_ACTION_TOG:
        /* Toggling supersedes any in-flight repeat. */
        data->repeat_action = PBL_ACTION_NONE;
        k_work_cancel_delayable(&data->repeat);
        data->screen_on = !data->screen_on;
        pbl_apply(data->screen_on ? data->brightness : 0);
        LOG_INF("Display toggled %s", data->screen_on ? "on" : "off");
        break;

    case PBL_ACTION_INC:
    case PBL_ACTION_DEC: {
        bool up = (binding->param1 == PBL_ACTION_INC);

        if (pbl_step(data, up)) {
            LOG_INF("Display brightness -> %d", data->brightness);
            /* Arm (or re-arm) auto-repeat for this key. */
            data->repeat_action = binding->param1;
            k_work_reschedule(&data->repeat, K_MSEC(PBL_REPEAT_DELAY_MS));
        }
        break;
    }

    default:
        LOG_WRN("Unknown brightness action %d", binding->param1);
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_pbl_data *data = dev->data;

    /* Stop repeating only if this is the key that owns the repeat; releasing
     * the other direction must not cancel a repeat the user is still holding. */
    if (data->repeat_action == binding->param1) {
        data->repeat_action = PBL_ACTION_NONE;
        k_work_cancel_delayable(&data->repeat);
    }

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
