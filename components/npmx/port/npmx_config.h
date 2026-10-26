/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file npmx_config.h
 * @brief PMIC 硬件型号配置桥接头文件（将用户的 npmx_esp32_config.h 映射至官方驱动）
 */

#ifndef NPMX_CONFIG_H__
#define NPMX_CONFIG_H__

#include "npmx_esp32_config.h"

#if (defined(NPMX_DEVICE_NPM1300) && NPMX_DEVICE_NPM1300) || defined(NPMX_ESP32_DEVICE_NPM1300)
    #ifndef NPM1300
    #define NPM1300 1
    #endif
    #include <npmx_config_npm1300.h>
#elif (defined(NPMX_DEVICE_NPM1304) && NPMX_DEVICE_NPM1304) || defined(NPMX_ESP32_DEVICE_NPM1304)
    #ifndef NPM1304
    #define NPM1304 1
    #endif
    #include <npmx_config_npm1304.h>
#else
    #error "未识别的 PMIC 器件型号！请在 npmx_esp32_config.h 中设置 NPMX_DEVICE_NPM1300 或 NPMX_DEVICE_NPM1304 为 1。"
#endif

#endif /* NPMX_CONFIG_H__ */
