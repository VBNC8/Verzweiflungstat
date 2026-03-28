/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

struct zmk_widget_battery_split {
    sys_snode_t node;
    lv_obj_t *obj;
};

int zmk_widget_battery_split_init(struct zmk_widget_battery_split *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_battery_split_obj(struct zmk_widget_battery_split *widget);
