cmake_minimum_required(VERSION 3.10)
project(meshui C)

set(CMAKE_C_STANDARD 11)

# lv_conf.h se trouve à la racine du projet, inclusion simple
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# LVGL doit être présent dans ./lvgl (cf. README : git clone + checkout release/v9.2)
add_subdirectory(lvgl)

add_executable(meshui main.c)
target_link_libraries(meshui lvgl lvgl_demos m pthread)
