cmake_minimum_required(VERSION 3.10)
project(meshui C)

set(CMAKE_C_STANDARD 11)

# lv_conf.h à la racine du projet
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# LVGL doit être présent dans ./lvgl (cf. README : clone + checkout release/v9.2)
add_subdirectory(lvgl)

file(GLOB APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_executable(meshui ${APP_SRC})
target_link_libraries(meshui lvgl m pthread)
