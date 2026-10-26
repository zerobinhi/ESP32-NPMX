/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file npmx_esp32_config.h
 * @brief npmx ESP-IDF 移植层的用户配置头文件
 *
 * 【重要说明】
 * 本工程完全遵循用户需求，不使用任何 Kconfig / idf.py menuconfig 选项！
 * 所有可配置项均以 C 语言宏（#define）的形式集中在本文件中。
 * 用户只需修改本头文件即可完成器件选型、引脚分配、通信速率与运行模式的定制。
 */

#ifndef NPMX_ESP32_CONFIG_H__
#define NPMX_ESP32_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 1. PMIC 目标型号选择（必须且只能启用一个型号）
 * -------------------------------------------------------------------------- */
/** @brief 选用 nPM1300 PMIC (设为 1 启用，0 禁用) */
#define NPMX_DEVICE_NPM1300                 1

/** @brief 选用 nPM1304 PMIC (设为 1 启用，0 禁用) */
#define NPMX_DEVICE_NPM1304                 0

/* 兼容性宏定义，同时支持 NPMX_ESP32_DEVICE_* 命名 */
#if NPMX_DEVICE_NPM1300
    #define NPMX_ESP32_DEVICE_NPM1300
#elif NPMX_DEVICE_NPM1304
    #define NPMX_ESP32_DEVICE_NPM1304
#endif

/* 编译期静态校验：确保仅选择了一个设备型号 */
#if (NPMX_DEVICE_NPM1300 + NPMX_DEVICE_NPM1304) != 1
    #error "配置错误：必须且只能在 NPMX_DEVICE_NPM1300 与 NPMX_DEVICE_NPM1304 中选择一个设为 1！"
#endif

/* --------------------------------------------------------------------------
 * 2. I2C 主机总线配置
 * --------------------------------------------------------------------------
 * 当调用 npmx_esp32_init_default() 或 npmx_esp32_init(NULL) 时，
 * 驱动内部将根据下列参数自动创建 I2C 主机总线。
 * 若用户已有外部共享的 I2C 总线，可通过 npmx_esp32_init(bus_handle) 传入，
 * 届时端口与 SDA/SCL 引脚配置将被跳过，仅使用下面的从机地址与速率。
 * -------------------------------------------------------------------------- */
/** @brief I2C 硬件端口号 (0 = I2C_NUM_0, 1 = I2C_NUM_1) */
#define NPMX_ESP32_I2C_PORT                 0

/** @brief ESP32 侧连接到 PMIC SDA 的 GPIO 编号 */
#define NPMX_ESP32_I2C_SDA_GPIO             21

/** @brief ESP32 侧连接到 PMIC SCL 的 GPIO 编号 */
#define NPMX_ESP32_I2C_SCL_GPIO             22

/** @brief nPM1300 / nPM1304 的 7 位 I2C 从机地址（硬件默认通常为 0x6B） */
#define NPMX_ESP32_I2C_ADDR                 0x6B

/** @brief I2C 时钟频率（Hz），标准模式 100 kHz (100000) 或快速模式 400 kHz (400000) */
#define NPMX_ESP32_I2C_FREQ_HZ              400000

/** @brief 单次 I2C 传输超时时间（单位：毫秒） */
#define NPMX_ESP32_I2C_TIMEOUT_MS           100

/** @brief 是否使能 ESP32 内部上拉电阻 (1 = 启用, 0 = 禁用) */
#define NPMX_ESP32_I2C_INTERNAL_PULLUP      1

/** @brief I2C 硬件滤波去毛刺采样点数 (通常设为 7) */
#define NPMX_ESP32_I2C_GLITCH_IGNORE_CNT     7

/* --------------------------------------------------------------------------
 * 3. 驱动核心行为配置
 * -------------------------------------------------------------------------- */
/**
 * @brief PMIC 寄存器值恢复策略 (对应 Zephyr 的 CONFIG_NPMX_RESTORE_VALUES)
 *   1 = 复位后保留 PMIC 内部现存寄存器值（利用 PMIC scratch 寄存器保持配置）
 *   0 = npmx_core_init() 时使用驱动库默认参数覆盖 PMIC 寄存器
 */
#define NPMX_ESP32_RESTORE_VALUES           1

/* --------------------------------------------------------------------------
 * 4. 电池与 NTC 热敏电阻配置
 * --------------------------------------------------------------------------
 * 【核心原理解释】
 * 许多开发板未连接电池或连接两线电池（无 NTC 引脚），此时 PMIC 的 NTC 引脚悬空。
 * 若开启 NTC 温度监控，PMIC 会因为悬空引脚而误判为低温/高温故障并强行禁止充电！
 *   0 = 两线电池或未接入电池（将 NTC 引脚设为高阻态并关闭 NTC 保护，允许充电器启动）
 *   1 = 三线电池（带 10kΩ NTC 热敏电阻，开启标准 JEITA 充放电温度保护）
 * -------------------------------------------------------------------------- */
/** @brief 是否具备 NTC 热敏电阻 (0 = 无 NTC / 两线电池 / 调试板, 1 = 有 NTC) */
#define NPMX_ESP32_BATTERY_HAS_NTC          0

/** @brief 锂电池充电终止电压（单位：毫伏 mV，常规单节锂电为 4200 mV） */
#define NPMX_ESP32_TERMINATION_VOLTAGE_MV   4200

/** @brief 恒流充电电流（单位：微安 uA，200000 uA = 200 mA） */
#define NPMX_ESP32_CHARGING_CURRENT_UA      200000

/* --------------------------------------------------------------------------
 * 5. 中断 (INT) 引脚配置 (PMIC -> ESP32)
 * -------------------------------------------------------------------------- */
/** @brief ESP32 侧接收中断的 GPIO 编号 (-1 表示不使用硬件中断引脚) */
#define NPMX_ESP32_INT_GPIO                 4

/** @brief PMIC 侧用作中断输出的 GPIO 编号 (取值 0..4，-1 表示不配置) */
#define NPMX_ESP32_PMIC_INT_PIN             0

/** @brief 中断电平有效性：1 = 高电平有效 (Active High)，0 = 低电平有效 (Active Low) */
#define NPMX_ESP32_INT_ACTIVE_HIGH          1

/* 校验中断引脚配置的一致性 */
#if (NPMX_ESP32_INT_GPIO >= 0) && (NPMX_ESP32_PMIC_INT_PIN < 0)
    #error "配置错误：当配置了 NPMX_ESP32_INT_GPIO 时，必须同时指定 NPMX_ESP32_PMIC_INT_PIN (0..4)！"
#endif

/* --------------------------------------------------------------------------
 * 6. 掉电预警 (POF / Power-Fail) 引脚配置
 * -------------------------------------------------------------------------- */
/** @brief ESP32 侧接收 POF 掉电预警的 GPIO 编号 (-1 表示禁用 POF) */
#define NPMX_ESP32_POF_GPIO                 (-1)

/** @brief PMIC 侧用作 PLW (Power Loss Warning) 输出的 GPIO 编号 (0..4，-1 表示不使用) */
#define NPMX_ESP32_PMIC_POF_PIN             (-1)

/* 校验 POF 引脚配置的一致性 */
#if (NPMX_ESP32_POF_GPIO >= 0) && (NPMX_ESP32_PMIC_POF_PIN < 0)
    #error "配置错误：当配置了 NPMX_ESP32_POF_GPIO 时，必须同时指定 NPMX_ESP32_PMIC_POF_PIN (0..4)！"
#endif

/* --------------------------------------------------------------------------
 * 7. PMIC 复位输出引脚配置 (可选)
 * -------------------------------------------------------------------------- */
/** @brief PMIC 侧配置为系统复位输出的 GPIO 编号 (0..4，-1 表示不配置) */
#define NPMX_ESP32_PMIC_RESET_PIN           (-1)

/* --------------------------------------------------------------------------
 * 8. FreeRTOS 事件处理任务配置（中断下半部）
 * -------------------------------------------------------------------------- */
/** @brief 事件处理任务的任务堆栈大小（单位：字节） */
#define NPMX_ESP32_TASK_STACK_SIZE          4096

/** @brief 事件处理任务的 FreeRTOS 任务优先级 */
#define NPMX_ESP32_TASK_PRIORITY            10

#ifdef __cplusplus
}
#endif

#endif /* NPMX_ESP32_CONFIG_H__ */
