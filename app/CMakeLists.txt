cmake_minimum_required(VERSION 3.10)
project(bq-lora-ui C)

set(CMAKE_C_STANDARD 11)

# ----------------------------------------------------------------------------
# Modules optionnels (toggle a la compilation).
# Defaut : tout active, sauf BME280 (capteur non encore cable / scaffold seul).
# Override en ligne de commande, ex :
#   cmake -S app -B build -DENABLE_CAMERA=OFF -DENABLE_BME280=ON
# ----------------------------------------------------------------------------
option(ENABLE_CAMERA    "Vue Camera (viseur live IMX219) + galerie photos" ON)
option(ENABLE_BLUETOOTH "Bluetooth scan/pair + console serie SPP"           ON)
option(ENABLE_BADUSB    "Bad USB (gadget HID/storage + explorateur)"        ON)
option(ENABLE_WIFI_QR   "Scanner QR WiFi (necessite ENABLE_CAMERA + libzbar)" ON)
option(ENABLE_WPS       "WPS push-button (wpa_cli wps_pbc)"                 ON)
option(ENABLE_BME280    "Capteur environnemental BME280 (I2C)"              OFF)

if(ENABLE_WIFI_QR AND NOT ENABLE_CAMERA)
    message(FATAL_ERROR "ENABLE_WIFI_QR requires ENABLE_CAMERA (le scanner partage le flux camera)")
endif()

# lv_conf.h a la racine du projet
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

# Defines de configuration vers le code (le header app/config.h applique les
# fallbacks par defaut si un flag n'est pas passe ici, pour rester safe quand
# on compile sans CMake). 1 = active, 0 = desactive.
foreach(mod CAMERA BLUETOOTH BADUSB WIFI_QR WPS BME280)
    if(ENABLE_${mod})
        add_compile_definitions(CFG_${mod}=1)
    else()
        add_compile_definitions(CFG_${mod}=0)
    endif()
endforeach()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# LVGL doit etre present dans ./lvgl (cf. README : clone + checkout release/v9.2)
add_subdirectory(lvgl)

file(GLOB APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
add_executable(bq-lora-ui ${APP_SRC})

# Libs externes conditionnelles
set(EXTRA_LIBS "")
if(ENABLE_WIFI_QR)
    # libzbar (apt: libzbar0 libzbar-dev) : decodage QR pour la conf WiFi.
    find_library(ZBAR_LIB zbar REQUIRED)
    list(APPEND EXTRA_LIBS ${ZBAR_LIB})
endif()

target_link_libraries(bq-lora-ui lvgl m pthread ${EXTRA_LIBS})

# Resume des options pour le log de build
message(STATUS "bq-lora-ui modules :")
message(STATUS "  CAMERA    = ${ENABLE_CAMERA}")
message(STATUS "  BLUETOOTH = ${ENABLE_BLUETOOTH}")
message(STATUS "  BADUSB    = ${ENABLE_BADUSB}")
message(STATUS "  WIFI_QR   = ${ENABLE_WIFI_QR}")
message(STATUS "  WPS       = ${ENABLE_WPS}")
message(STATUS "  BME280    = ${ENABLE_BME280}")
