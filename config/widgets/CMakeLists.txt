# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources(app PRIVATE battery_split.c battery_split_widget.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
