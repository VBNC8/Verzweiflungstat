/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#include <lvgl.h>
#include <stdio.h>

#include "battery_split.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct battery_split_state {
    uint8_t local_battery;
    uint8_t remote_battery;
    bool remote_valid;
};

static void set_battery_split_symbol(lv_obj_t *label, struct battery_split_state state) {
    char text[20] = {};

    if (state.remote_valid) {
        snprintf(text, sizeof(text), "L:%u%% R:%u%%", state.local_battery, state.remote_battery);
    } else {
        snprintf(text, sizeof(text), "L:%u%%  R:?", state.local_battery);
    }

    lv_label_set_text(label, text);
}

void battery_split_update_cb(struct battery_split_state state) {
    struct zmk_widget_battery_split *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_battery_split_symbol(widget->obj, state);
    }
}

static struct battery_split_state battery_split_get_state(const zmk_event_t *eh) {
    uint8_t local_level = zmk_battery_state_of_charge();
    uint8_t remote_level = 0;
    bool remote_valid = false;

    const struct zmk_battery_state_changed *lev = as_zmk_battery_state_changed(eh);
    if (lev != NULL) {
        local_level = lev->state_of_charge;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *pev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (pev != NULL && pev->source == 0) {
        remote_level = pev->state_of_charge;
        remote_valid = true;
    } else {
        int rc = zmk_split_central_get_peripheral_battery_level(0, &remote_level);
        remote_valid = (rc == 0);
    }
#endif

    return (struct battery_split_state){
        .local_battery = local_level,
        .remote_battery = remote_level,
        .remote_valid = remote_valid,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_split, struct battery_split_state,
                            battery_split_update_cb, battery_split_get_state)

ZMK_SUBSCRIPTION(widget_battery_split, zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(widget_battery_split, zmk_peripheral_battery_state_changed);
#endif

int zmk_widget_battery_split_init(struct zmk_widget_battery_split *widget, lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);
    lv_label_set_text(widget->obj, "L:--% R:--");

    sys_slist_append(&widgets, &widget->node);

    widget_battery_split_init();
    return 0;
}

lv_obj_t *zmk_widget_battery_split_obj(struct zmk_widget_battery_split *widget) {
    return widget->obj;
}
