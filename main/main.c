/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file main.c
 * @brief nPM1300 / nPM1304 PMIC 在 ESP32 上的完整示例工程 (最终综合版)
 *
 * 本程序演示：
 *   1. 初始化 npmx ESP-IDF 移植层（I2C 总线建立、PMIC 芯片检测、中断与下半部启动）
 *   2. 查询并打印芯片固件版本及硬件信息
 *   3. 配置降压稳压器 (BUCK1 / BUCK2) 输出电压（附板级供电安全说明）
 *   4. 配置低压降稳压器 / 负载开关 (LDSW1) 为 3.3V LDO 供电模式
 *   5. 配置智能锂电池线性充电器（设置恒流充电电流为 200mA 并启用充电与 NTC 温度保护）
 *   6. 配置 PMIC 状态指示 LED（硬件自动联动充电状态）
 *   7. 订阅 PMIC 硬件中断事件（VBUS 插入/拔出、电池接入/移除、充电完成/告警）
 *   8. 启动 FreeRTOS 周期监控任务，周期性采样电池电压 (VBAT)、系统电压 (VSYS)、内部结温及充电状态
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 引入 npmx ESP32 移植层统一头文件 */
#include "npmx_esp32.h"

static const char *TAG = "npmx_app";

/** @brief 周期监控任务采样间隔（毫秒） */
#define MONITOR_SAMPLE_PERIOD_MS    2000

/* --------------------------------------------------------------------------
 * 1. PMIC 中断事件回调函数
 * --------------------------------------------------------------------------
 * 说明：此回调函数在 npmx 移植层的后台事件任务 (npmx_evt) 上下文中执行，
 *       已脱离 ISR 硬中断上下文，因此在回调中可以安全地进行 I2C 读写操作。
 * -------------------------------------------------------------------------- */

static void on_pmic_event(npmx_instance_t *p_pm, npmx_callback_type_t type, uint8_t mask)
{
    (void)p_pm;

    ESP_LOGI(TAG, ">>> 收到 PMIC 硬件事件: %s [掩码: 0x%02X]",
             npmx_callback_to_str(type), mask);

    for (uint8_t i = 0; i < 8; i++) {
        if (mask & (1U << i)) {
            ESP_LOGI(TAG, "    * 详情: %s", npmx_callback_bit_to_str(type, i));
        }
    }
}

/* --------------------------------------------------------------------------
 * 2. 硬件外设模块初始化与配置演示
 * -------------------------------------------------------------------------- */

/**
 * @brief 配置 BUCK 降压转换器
 *
 * 【硬件安全提示】
 * 若硬件板级设计中，ESP32 自身的 3.3V 供电是由 PMIC 的 BUCK1 输出提供的，
 * 切勿在代码中将 BUCK1 电压随意修改为低于 3.0V 或高于 3.6V，否则会导致 ESP32
 * 欠压复位或过压烧毁！
 */
static void setup_buck_converters(npmx_instance_t *pmic)
{
    ESP_LOGI(TAG, "正在配置 BUCK 降压转换器...");

    /* 配置 BUCK1: 3.0V (3000 mV) */
    npmx_buck_t *buck1 = npmx_buck_get(pmic, 0);
    if (buck1 != NULL) {
        npmx_buck_voltage_t v1 = npmx_buck_voltage_convert(3000);
        npmx_error_t err = npmx_buck_normal_voltage_set(buck1, v1);
        if (err == NPMX_SUCCESS) {
            ESP_LOGI(TAG, "BUCK1 输出目标电压已配置为: 3000 mV");
        } else {
            ESP_LOGE(TAG, "BUCK1 配置失败: %s", npmx_esp32_error_to_str(err));
        }
    }

    /* 配置 BUCK2: 1.8V (1800 mV，常用于外部低压传感器或辅助芯片供电) */
    npmx_buck_t *buck2 = npmx_buck_get(pmic, 1);
    if (buck2 != NULL) {
        npmx_buck_voltage_t v2 = npmx_buck_voltage_convert(1800);
        npmx_error_t err = npmx_buck_normal_voltage_set(buck2, v2);
        if (err == NPMX_SUCCESS) {
            ESP_LOGI(TAG, "BUCK2 输出目标电压已配置为: 1800 mV");
        } else {
            ESP_LOGE(TAG, "BUCK2 配置失败: %s", npmx_esp32_error_to_str(err));
        }
    }
}

/**
 * @brief 配置 LDSW1 为 LDO 输出模式 (3.3V)
 */
static void setup_ldsw(npmx_instance_t *pmic)
{
    ESP_LOGI(TAG, "正在配置 LDSW1 / LDO 输出...");
    npmx_ldsw_t *ldsw = npmx_ldsw_get(pmic, 0);
    if (ldsw == NULL) {
        return;
    }

    /* 配置 LDSW1 工作模式为高 PSRR 的 LDO 稳压器模式 */
    npmx_error_t err = npmx_ldsw_mode_set(ldsw, NPMX_LDSW_MODE_LDO);
    if (err != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "设置 LDSW1 为 LDO 模式失败: %s", npmx_esp32_error_to_str(err));
        return;
    }

    /* 配置 LDO 输出电压为 3.3V (3300 mV) */
    npmx_ldsw_voltage_t v = npmx_ldsw_voltage_convert(3300);
    err = npmx_ldsw_ldo_voltage_set(ldsw, v);
    if (err != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "设置 LDSW1 输出电压失败: %s", npmx_esp32_error_to_str(err));
        return;
    }

    /* 启动使能 LDSW1 输出 */
    err = npmx_ldsw_task_trigger(ldsw, NPMX_LDSW_TASK_ENABLE);
    if (err == NPMX_SUCCESS) {
        ESP_LOGI(TAG, "LDSW1 (3.3V LDO) 输出已成功开启");
    } else {
        ESP_LOGE(TAG, "使能 LDSW1 输出失败: %s", npmx_esp32_error_to_str(err));
    }
}

/**
 * @brief 配置智能锂电池线性充电器参数
 */
static void setup_charger(npmx_instance_t *pmic)
{
    ESP_LOGI(TAG, "正在配置锂电池充电器...");
    npmx_charger_t *charger = npmx_charger_get(pmic, 0);
    if (charger == NULL) {
        return;
    }

    /* 修改关键充电参数前先暂停充电模块 */
    npmx_error_t err = npmx_charger_module_disable_set(charger, NPMX_CHARGER_MODULE_CHARGER_MASK);
    if (err != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "暂停充电器模块失败: %s", npmx_esp32_error_to_str(err));
        return;
    }

    /* 配置恒流充电电流为 200 mA (200000 uA) */
    err = npmx_charger_charging_current_set(charger, 200000);
    if (err != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "设置恒流充电电流失败: %s", npmx_esp32_error_to_str(err));
        return;
    }

    /* 重新使能充电功能、自动复充及 NTC 电池温度极限保护 */
    err = npmx_charger_module_enable_set(charger,
                                         NPMX_CHARGER_MODULE_CHARGER_MASK |
                                         NPMX_CHARGER_MODULE_RECHARGE_MASK |
                                         NPMX_CHARGER_MODULE_NTC_LIMITS_MASK);
    if (err == NPMX_SUCCESS) {
        uint32_t current_ua = 0;
        npmx_charger_charging_current_get(charger, &current_ua);
        ESP_LOGI(TAG, "充电器使能成功！当前设定充电电流: %" PRIu32 " mA", current_ua / 1000);
    } else {
        ESP_LOGE(TAG, "重新启用充电器模块失败: %s", npmx_esp32_error_to_str(err));
    }
}

/**
 * @brief 配置 PMIC 板载 LED 指示灯
 */
static void setup_led(npmx_instance_t *pmic)
{
    npmx_led_t *led = npmx_led_get(pmic, 0);
    if (led != NULL) {
        /* 配置 LED0 为充电状态硬件联动模式（充电时呼吸/常亮，充满后指示） */
        npmx_error_t err = npmx_led_mode_set(led, NPMX_LED_MODE_CHARGING);
        if (err == NPMX_SUCCESS) {
            ESP_LOGI(TAG, "PMIC LED0 已配置为硬件充电指示模式");
        }
    }
}

/**
 * @brief 订阅 PMIC 常用硬件事件中断
 */
static void setup_event_subscriptions(void)
{
    ESP_LOGI(TAG, "正在注册 PMIC 事件中断订阅...");

    /* 1. 注册 VBUS 电源输入事件回调（插入/拔出检测） */
    npmx_esp32_register_cb(NPMX_CALLBACK_TYPE_EVENT_VBUSIN_VOLTAGE, on_pmic_event);
    npmx_esp32_event_interrupt_enable(
        NPMX_EVENT_GROUP_VBUSIN_VOLTAGE,
        NPMX_EVENT_GROUP_VBUSIN_DETECTED_MASK | NPMX_EVENT_GROUP_VBUSIN_REMOVED_MASK);

    /* 2. 注册电池充电器状态事件回调（充满/充电错误） */
    npmx_esp32_register_cb(NPMX_CALLBACK_TYPE_EVENT_BAT_CHAR_STATUS, on_pmic_event);
    npmx_esp32_event_interrupt_enable(
        NPMX_EVENT_GROUP_BAT_CHAR_STATUS,
        NPMX_EVENT_GROUP_CHARGER_COMPLETED_MASK | NPMX_EVENT_GROUP_CHARGER_ERROR_MASK);

    /* 3. 注册电池在位/移除事件回调 */
    npmx_esp32_register_cb(NPMX_CALLBACK_TYPE_EVENT_BAT_CHAR_BAT, on_pmic_event);
    npmx_esp32_event_interrupt_enable(
        NPMX_EVENT_GROUP_BAT_CHAR_BAT,
        NPMX_EVENT_GROUP_BATTERY_DETECTED_MASK | NPMX_EVENT_GROUP_BATTERY_REMOVED_MASK);

    ESP_LOGI(TAG, "PMIC 事件中断订阅配置完成！");
}

/* --------------------------------------------------------------------------
 * 3. FreeRTOS 周期监控任务
 * -------------------------------------------------------------------------- */

static void pmic_monitor_task(void *arg)
{
    (void)arg;
    npmx_instance_t *pmic = npmx_esp32_instance_get();
    npmx_charger_t *charger = npmx_charger_get(pmic, 0);

    ESP_LOGI(TAG, "PMIC 周期监控任务已成功启动！");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_SAMPLE_PERIOD_MS));

        /* 采集电池电压 VBAT */
        int32_t vbat_mv = 0;
        npmx_error_t err_vbat = npmx_esp32_vbat_get_mv(&vbat_mv);

        /* 采集系统供电总线电压 VSYS */
        int32_t vsys_mv = 0;
        npmx_error_t err_vsys = npmx_esp32_vsys_get_mv(&vsys_mv);

        /* 采集 PMIC 芯片内部结温 */
        float die_temp_c = 0.0f;
        npmx_error_t err_temp = npmx_esp32_die_temp_get_c(&die_temp_c);

        /* 读取实时充电器状态 */
        uint8_t chg_status = 0;
        npmx_esp32_lock();
        npmx_error_t err_chg = npmx_charger_status_get(charger, &chg_status);
        npmx_esp32_unlock();

        /* 格式化输出监控数据 */
        ESP_LOGI(TAG, "------------------ PMIC 实时遥测数据 ------------------");
        if (err_vbat == NPMX_SUCCESS) {
            ESP_LOGI(TAG, " [电池电压 VBAT]  : %" PRId32 " mV", vbat_mv);
        } else {
            ESP_LOGW(TAG, " [电池电压 VBAT]  : 采集超时或失败 (%s)", npmx_esp32_error_to_str(err_vbat));
        }

        if (err_vsys == NPMX_SUCCESS) {
            ESP_LOGI(TAG, " [系统供电 VSYS]  : %" PRId32 " mV", vsys_mv);
        } else {
            ESP_LOGW(TAG, " [系统供电 VSYS]  : 采集超时或失败 (%s)", npmx_esp32_error_to_str(err_vsys));
        }

        if (err_temp == NPMX_SUCCESS) {
            ESP_LOGI(TAG, " [PMIC 芯片结温]  : %.2f ℃", die_temp_c);
        } else {
            ESP_LOGW(TAG, " [PMIC 芯片结温]  : 采集超时或失败 (%s)", npmx_esp32_error_to_str(err_temp));
        }

        if (err_chg == NPMX_SUCCESS) {
            ESP_LOGI(TAG, " [充电器状态码]   : 0x%02X (%s%s%s)",
                     chg_status,
                     (chg_status & NPMX_CHARGER_STATUS_BATTERY_DETECTED_MASK) ? "电池已接入 " : "无电池 ",
                     (chg_status & NPMX_CHARGER_STATUS_CONSTANT_CURRENT_MASK) ? "恒流充电中 " : "",
                     (chg_status & NPMX_CHARGER_STATUS_COMPLETED_MASK)        ? "充电已完成" : "未在充电");
        }
        ESP_LOGI(TAG, "--------------------------------------------------------");
    }
}

/* --------------------------------------------------------------------------
 * 4. 主程序入口 (app_main)
 * -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, "    ESP32 - nPM1300/nPM1304 PMIC 移植工程 (最终综合版)    ");
    ESP_LOGI(TAG, "========================================================");

    /* 1. 初始化 PMIC 移植层（I2C 总线自动建立，根据 npmx_esp32_config.h 中的 #define 驱动） */
    esp_err_t err = npmx_esp32_init_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC 初始化失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "请检查 PMIC 接线 (SDA/SCL/VBUS/GND) 及从机地址配置！");
        return;
    }

    npmx_instance_t *pmic = npmx_esp32_instance_get();

    /* 2. 打印当前芯片状态概览 */
    npmx_esp32_dump_status();

    /* 3. 配置外设模块：BUCK 降压输出、LDSW 供电、充电器、指示 LED */
    setup_buck_converters(pmic);
    setup_ldsw(pmic);
    setup_charger(pmic);
    setup_led(pmic);

    /* 4. 配置并订阅硬件中断事件 */
    setup_event_subscriptions();

    /* 5. 创建 FreeRTOS 周期监控任务 */
    xTaskCreate(pmic_monitor_task, "pmic_mon", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "系统初始化全部完成，所有 PMIC 驱动服务运行正常！");
}
