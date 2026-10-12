/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_AURORA,
    UNDERGLOW_EFFECT_EMBERS,
    UNDERGLOW_EFFECT_COMET,
    UNDERGLOW_EFFECT_HEARTBEAT,
    UNDERGLOW_EFFECT_TYPING,
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_rgb_underglow_effect_solid(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

#define FX_NO_ZONE 0xFF

/*
 * LED chain index lit by each key position, used by the typing effect.
 * Specific to the eyelash_sofle: its per-key LEDs are wired per column, so each
 * half only has STRIP_NUM_PIXELS addressable zones (6 columns + thumb keys).
 */
static const uint8_t fx_key_zone[] = {
    0, 1, 2, 3, 4, 5, FX_NO_ZONE, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, FX_NO_ZONE, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, FX_NO_ZONE, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, FX_NO_ZONE, 5, 4, 3, 2, 1, 0,
    FX_NO_ZONE, 2, 3, 4, 6, 6, FX_NO_ZONE, 6, 6, 4, 3, 2,
};

// Effect scratch state. Kept out of rgb_underglow_state, whose size is checked when loading
// settings.
static uint8_t fx_heat[STRIP_NUM_PIXELS];
static uint8_t fx_spark[STRIP_NUM_PIXELS];
static uint16_t fx_energy;
static uint32_t fx_rand_state;

static void fx_reset(void) {
    memset(fx_heat, 0, sizeof(fx_heat));
    memset(fx_spark, 0, sizeof(fx_spark));
    fx_energy = 0;
}

static int fx_rand(void) {
    if (fx_rand_state == 0) {
        // Seeded per device so the two halves of a split don't flicker in unison.
        fx_rand_state = sys_rand32_get() | 1;
    }

    fx_rand_state ^= fx_rand_state << 13;
    fx_rand_state ^= fx_rand_state >> 17;
    fx_rand_state ^= fx_rand_state << 5;

    return fx_rand_state & 0x7FFFFFFF;
}

// Triangle wave: 0..100..0 over one period.
static int fx_tri(int x, int period) {
    int half = period / 2;

    x %= period;

    return (x < half ? x : period - x) * 100 / half;
}

// Smoothstep on 0..100.
static int fx_ease(int t) { return t * t * (300 - 2 * t) / 10000; }

static void zmk_rgb_underglow_effect_aurora(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        int hue_wave = fx_ease(fx_tri(state.animation_step + i * 360, 3600));
        int brt_wave = fx_ease(fx_tri(state.animation_step * 3 + i * 500, 3600));

        hsb.h = (state.color.h + hue_wave * 110 / 100) % HUE_MAX;
        hsb.b = state.color.b * (45 + brt_wave * 55 / 100) / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step = (state.animation_step + state.animation_speed * 4) % 3600;
}

static void zmk_rgb_underglow_effect_embers(void) {
    uint8_t spread[STRIP_NUM_PIXELS];

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        int cool = fx_rand() % (6 + state.animation_speed * 3);
        int heat = fx_heat[i] > cool ? fx_heat[i] - cool : 0;

        if (fx_rand() % 100 < 3 + state.animation_speed * 2) {
            int flare = 35 + fx_rand() % 45;

            heat = MIN(100, heat + flare);
        }
        fx_heat[i] = heat;

        hsb.h = heat * 38 / 100;
        hsb.s = SAT_MAX;
        hsb.b = state.color.b * (22 + heat * 78 / 100) / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int left = fx_heat[i > 0 ? i - 1 : i];
        int right = fx_heat[i < STRIP_NUM_PIXELS - 1 ? i + 1 : i];

        spread[i] = (left + fx_heat[i] * 6 + right) / 8;
    }
    memcpy(fx_heat, spread, sizeof(fx_heat));
}

static void zmk_rgb_underglow_effect_comet(void) {
    const int length = STRIP_NUM_PIXELS * 100;
    const int tail = 350;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        int behind = (state.animation_step - i * 100 + length) % length;
        int brt = 0;

        if (behind < tail) {
            brt = (tail - behind) * 100 / tail;
            brt = brt * brt / 100;
        } else if (behind > length - 100) {
            brt = behind - (length - 100);
        }

        if (brt > 70) {
            hsb.s = state.color.s * (100 - (brt - 70) * 2) / 100;
        }
        hsb.b = state.color.b * brt / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step = (state.animation_step + state.animation_speed * 8) % length;
}

// Envelope of one "lub-dub" over a 2000 step cycle, 0..100.
static int fx_beat(int t) {
    if (t < 100) {
        return t;
    }
    if (t < 450) {
        return 100 - (t - 100) * 80 / 350;
    }
    if (t < 550) {
        return 20 + (t - 450) * 50 / 100;
    }
    if (t < 1100) {
        return 70 - (t - 550) * 70 / 550;
    }

    return 0;
}

static void zmk_rgb_underglow_effect_heartbeat(void) {
    const int middle = STRIP_NUM_PIXELS / 2;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        int distance = abs(i - middle);
        int beat = fx_beat((state.animation_step + 2000 - distance * 70) % 2000) *
                   (100 - distance * 14) / 100;

        hsb.h = 350;
        hsb.s = SAT_MAX - beat * 25 / 100;
        hsb.b = state.color.b * (6 + beat * 94 / 100) / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step = (state.animation_step + state.animation_speed * 30) % 2000;
}

static void zmk_rgb_underglow_effect_typing(void) {
    int energy = fx_energy;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        int idle = fx_tri(state.animation_step + i * 150, 1000) * 10 / 100;
        int base = 12 + idle + energy * 60 / 1000;
        bool spark = fx_spark[i] > base;

        hsb.h = (state.color.h + energy * 140 / 1000) % HUE_MAX;
        if (energy > 700) {
            hsb.s = state.color.s * (100 - (energy - 700) * 40 / 300) / 100;
        }
        if (spark) {
            hsb.s /= 2;
        }
        hsb.b = state.color.b * MIN(100, spark ? fx_spark[i] : base) / 100;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));

        fx_spark[i] = fx_spark[i] > 18 ? fx_spark[i] - 18 : 0;
    }

    energy -= energy * (state.animation_speed + 1) / 40 + 1;
    fx_energy = MAX(energy, 0);

    state.animation_step = (state.animation_step + state.animation_speed * 5) % 1000;
}

static int rgb_underglow_typing_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state && state.on && state.current_effect == UNDERGLOW_EFFECT_TYPING) {
        uint8_t zone =
            ev->position < ARRAY_SIZE(fx_key_zone) ? fx_key_zone[ev->position] : FX_NO_ZONE;

        if (zone >= STRIP_NUM_PIXELS) {
            zone = fx_rand() % STRIP_NUM_PIXELS;
        }

        fx_spark[zone] = 100;
        fx_energy = MIN(1000, fx_energy + 220);
    }

    // Only observing: the event must keep bubbling or the key press is lost.
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rgb_underglow_typing, rgb_underglow_typing_listener);
ZMK_SUBSCRIPTION(rgb_underglow_typing, zmk_position_state_changed);

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
    case UNDERGLOW_EFFECT_AURORA:
        zmk_rgb_underglow_effect_aurora();
        break;
    case UNDERGLOW_EFFECT_EMBERS:
        zmk_rgb_underglow_effect_embers();
        break;
    case UNDERGLOW_EFFECT_COMET:
        zmk_rgb_underglow_effect_comet();
        break;
    case UNDERGLOW_EFFECT_HEARTBEAT:
        zmk_rgb_underglow_effect_heartbeat();
        break;
    case UNDERGLOW_EFFECT_TYPING:
        zmk_rgb_underglow_effect_typing();
        break;
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
            }

            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    }

    return 0;
}

int zmk_rgb_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

int zmk_rgb_underglow_on(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));

    return zmk_rgb_underglow_save_state();
}

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

int zmk_rgb_underglow_off(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&underglow_tick);
    state.on = false;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;
    fx_reset();

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_hue(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_sat(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_brt(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
struct rgb_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};

static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct rgb_underglow_sleep_state sleep_state = {
        is_awake : true,
        rgb_state_before_sleeping : false
    };

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_rgb_underglow_on();
        } else {
            return zmk_rgb_underglow_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_underglow_off();
    }
}

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
