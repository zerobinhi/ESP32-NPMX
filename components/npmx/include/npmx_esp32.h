/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file npmx_esp32.h
 * @brief npmx PMIC 官方驱动库在 ESP-IDF (FreeRTOS) 下的完整移植接口
 *
 * 本驱动为 Nordic Semiconductor nPM1300 / nPM1304 电源管理芯片 (PMIC)
 * 在乐鑫 ESP32 / ESP-IDF 平台上的官方级别移植实现。
 *
 * 核心技术特色：
 *   1. 纯宏定义配置：不依赖任何 Kconfig，所有参数集中在 npmx_esp32_config.h 中以 #define 配置。
 *   2. 基于 ESP-IDF v5 新版 i2c_master 驱动架构，具备总线所有权自主管理与外部共享能力。
 *   3. 完备的线程安全控制（递归互斥锁），彻底杜绝多任务并发及中断事件嵌套操作引起的死锁。
 *   4. FreeRTOS 中断下半部架构：GPIO 电平中断防风暴 + 任务通知 (Task Notification) 异步事件调度。
 *   5. 提供高度封装的实用便捷 API（VBAT/VSYS 电压采集、结温读取、状态一键格式化打印），
 *      同时完整透传原生 npmx_instance_t 句柄，可直接调用全部官方底层外设驱动。
 */

#ifndef NPMX_ESP32_H__
#define NPMX_ESP32_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* 引入纯 #define 用户配置头文件 */
#include "npmx_esp32_config.h"

/* 引入 npmx 官方核心驱动与外设模块头文件 */
#include <npmx.h>
#include <npmx_common.h>
#include <npmx_core.h>
#include <npmx_event.h>
#include <npmx_instance.h>
#include <npmx_pof.h>
#include <npmx_buck.h>
#include <npmx_charger.h>
#include <npmx_ldsw.h>
#include <npmx_led.h>
#include <npmx_adc.h>
#include <npmx_gpio.h>
#include <npmx_timer.h>
#include <npmx_ship.h>
#include <npmx_errlog.h>
#include <npmx_vbusin.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief POF（掉电预警）紧急回调函数指针类型
 * @note  回调在事件任务上下文中执行，可安全调用 I2C 读写及 PMIC 寄存器配置。
 */
typedef void (*npmx_esp32_pof_cb_t)(npmx_instance_t *p_pm);

/**
 * @brief 初始化 PMIC 移植层驱动（指定或新建 I2C 总线）
 *
 * @param[in] bus 已创建的 I2C 主机总线句柄（若传 NULL，则由本驱动根据配置头文件自动创建独立总线）
 *
 * @retval ESP_OK                初始化成功
 * @retval ESP_ERR_INVALID_STATE 驱动已处于初始化状态
 * @retval ESP_ERR_NO_MEM        系统内存不足（互斥锁或事件任务创建失败）
 * @retval ESP_ERR_NOT_FOUND     I2C 总线上未探测到 PMIC 芯片（请检查硬件接线、上拉电阻及从机地址）
 * @retval ESP_ERR_INVALID_ARG   引脚配置参数不合法
 * @retval 其它                  ESP-IDF 底层 I2C / GPIO 驱动错误码
 */
esp_err_t npmx_esp32_init(i2c_master_bus_handle_t bus);

/**
 * @brief 使用配置头文件中的默认参数自动创建 I2C 总线并初始化 PMIC 驱动
 *
 * @return 错误码，ESP_OK 表示成功
 */
esp_err_t npmx_esp32_init_default(void);

/**
 * @brief 释放 PMIC 移植层占用的全部软硬件资源
 *
 * 销毁 I2C 设备（若由本驱动创建则同时注销 I2C 总线）、移除 GPIO 中断处理函数并终止事件任务。
 *
 * @return 错误码，ESP_OK 表示成功
 */
esp_err_t npmx_esp32_deinit(void);

/**
 * @brief 查询 PMIC 移植层是否已就绪且通信正常
 *
 * @return true 驱动已就绪且芯片在线，false 未初始化或初始化失败
 */
bool npmx_esp32_is_ready(void);

/**
 * @brief 获取 npmx 核心实例指针
 *
 * 获取到的指针可直接传递给 npmx 官方外设驱动 API，例如：
 *   - npmx_buck_get(npmx_esp32_instance_get(), 0)
 *   - npmx_charger_get(npmx_esp32_instance_get(), 0)
 *   - npmx_adc_get(npmx_esp32_instance_get(), 0)
 *
 * @return npmx 实例指针，未初始化时返回 NULL
 */
npmx_instance_t *npmx_esp32_instance_get(void);

/**
 * @brief 获取当前移植层使用的 I2C 主机总线句柄
 */
i2c_master_bus_handle_t npmx_esp32_i2c_bus_get(void);

/**
 * @brief 获取当前移植层使用的 I2C 设备句柄
 */
i2c_master_dev_handle_t npmx_esp32_i2c_dev_get(void);

/**
 * @brief 获取 PMIC 访问递归互斥锁
 *
 * 当应用层有多个 FreeRTOS 任务同时操作 PMIC 时，可以使用本函数将一系列连续读写包裹起来，
 * 保证事务的原子性，同时防止被后台事件任务打断。
 */
void npmx_esp32_lock(void);

/**
 * @brief 释放 PMIC 访问递归互斥锁
 */
void npmx_esp32_unlock(void);

/**
 * @brief 注册特定类型的 PMIC 事件回调
 *
 * @param[in] type 回调类型，详见 npmx_callback_type_t
 * @param[in] cb   回调处理函数（传 NULL 表示取消注册）
 *
 * @retval ESP_OK                注册成功
 * @retval ESP_ERR_INVALID_STATE 移植层未就绪
 * @retval ESP_ERR_INVALID_ARG   无效的回调类型
 */
esp_err_t npmx_esp32_register_cb(npmx_callback_type_t type, npmx_callback_t cb);

/**
 * @brief 使能 PMIC 指定事件组的中断输出
 *
 * @param[in] group 事件组，详见 npmx_event_group_t
 * @param[in] mask  事件位掩码，例如 NPMX_EVENT_GROUP_ALL_EVENTS_MASK
 *
 * @retval ESP_OK                操作成功
 * @retval ESP_ERR_INVALID_STATE 移植层未就绪
 * @retval ESP_ERR_NOT_SUPPORTED 硬件未配置中断引脚
 * @retval ESP_FAIL              PMIC 寄存器写入失败
 */
esp_err_t npmx_esp32_event_interrupt_enable(npmx_event_group_t group, uint8_t mask);

/**
 * @brief 关闭并清除 PMIC 指定事件组的中断输出
 *
 * @param[in] group 事件组，详见 npmx_event_group_t
 * @param[in] mask  事件位掩码
 *
 * @return 错误码，ESP_OK 表示成功
 */
esp_err_t npmx_esp32_event_interrupt_disable(npmx_event_group_t group, uint8_t mask);

/**
 * @brief 配置 POF（掉电预警）比较器并注册预警回调
 *
 * @param[in] config POF 比较器配置结构体
 * @param[in] cb     触发掉电预警时的紧急回调函数
 *
 * @retval ESP_OK                配置成功
 * @retval ESP_ERR_INVALID_STATE 移植层未就绪
 * @retval ESP_ERR_INVALID_ARG   空指针参数
 * @retval ESP_ERR_NOT_SUPPORTED 未配置 POF 相关引脚
 * @retval ESP_FAIL              PMIC 写入失败
 */
esp_err_t npmx_esp32_register_pof_cb(const npmx_pof_config_t *config, npmx_esp32_pof_cb_t cb);

/**
 * @brief POF 触发后重新布防中断输入
 *
 * 当发生电源跌落预警时，为防止中断风暴，移植层会自动禁用 GPIO 中断；
 * 当供电恢复平稳后，调用本函数可重新开启中断监听。
 */
esp_err_t npmx_esp32_pof_rearm(void);

/**
 * @brief 将 npmx 错误码转换为可读的中文字符串
 *
 * @param[in] err npmx 错误码
 * @return 对应的中文字符串常量
 */
const char *npmx_esp32_error_to_str(npmx_error_t err);

/* --------------------------------------------------------------------------
 * 便捷操作函数（封装常用 ADC 采样与状态监控）
 * -------------------------------------------------------------------------- */

/**
 * @brief 便捷函数：单次采样电池电压 (VBAT)
 *
 * @param[out] p_vbat_mv 采样结果输出指针（单位：毫伏 mV）
 * @return npmx 错误码，NPMX_SUCCESS 表示成功
 */
npmx_error_t npmx_esp32_vbat_get_mv(int32_t *p_vbat_mv);

/**
 * @brief 便捷函数：单次采样系统供电电压 (VSYS)
 *
 * @param[out] p_vsys_mv 采样结果输出指针（单位：毫伏 mV）
 * @return npmx 错误码，NPMX_SUCCESS 表示成功
 */
npmx_error_t npmx_esp32_vsys_get_mv(int32_t *p_vsys_mv);

/**
 * @brief 便捷函数：单次采样芯片内部结温 (Die Temperature)
 *
 * @param[out] p_temp_c 采样结果输出指针（单位：摄氏度 ℃）
 * @return npmx 错误码，NPMX_SUCCESS 表示成功
 */
npmx_error_t npmx_esp32_die_temp_get_c(float *p_temp_c);

/**
 * @brief 便捷函数：打印 PMIC 全面状态信息（芯片版本、BUCK状态、充电器、VBUS等）
 */
void npmx_esp32_dump_status(void);

#ifdef __cplusplus
}
#endif

#endif /* NPMX_ESP32_H__ */
