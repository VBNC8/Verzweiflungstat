cmake_minimum_required(VERSION 3.20)

if(CONFIG_HEARTBEAT_LED)
  target_sources(app PRIVATE src/status_led.c)
endif()
