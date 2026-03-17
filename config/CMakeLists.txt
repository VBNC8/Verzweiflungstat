zephyr_library()
zephyr_library_sources(src/pinnacle.c)

# Das ist die entscheidende Zeile für die Header-Suche:
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../zmk/app/include)
