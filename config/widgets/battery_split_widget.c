/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <lvgl.h>

#include "battery_split.h"

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
#include <zmk/display/widgets/layer_status.h>
static struct zmk_widget_layer_status layer_status_widget;
#endif

static struct zmk_widget_battery_split battery_split_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen;
    screen = lv_obj_create(NULL);

    zmk_widget_battery_split_init(&battery_split_widget, screen);
    lv_obj_align(zmk_widget_battery_split_obj(&battery_split_widget), LV_ALIGN_TOP_MID, 0, 5);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_BOTTOM_LEFT, 0, 0);
#endif

    return screen;
}

