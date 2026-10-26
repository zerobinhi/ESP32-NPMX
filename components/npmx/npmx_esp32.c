/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file npmx_esp32.c
 * @brief npmx PMIC 驱动在 ESP-IDF (FreeRTOS) 下的移植层实现
 */

#include "npmx_esp32.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "npmx_esp32";

/** @brief 一次 I2C 寄存器传输的最大有效载荷字节数（npmx 驱动层通常为 1~4 字节） */
#define NPMX_ESP32_MAX_XFER                 32

/** @brief 事件任务通知位掩码定义（基于 FreeRTOS 任务通知，零内存开销） */
#define NPMX_ESP32_NOTIF_INT                (1U << 0)
#define NPMX_ESP32_NOTIF_POF                (1U << 1)

#if (NPMX_ESP32_INT_GPIO >= 0)
    #define NPMX_ESP32_HAS_INT              1
#else
    #define NPMX_ESP32_HAS_INT              0
#endif

#if (NPMX_ESP32_POF_GPIO >= 0)
    #define NPMX_ESP32_HAS_POF              1
#else
    #define NPMX_ESP32_HAS_POF              0
#endif

#if NPMX_ESP32_INT_ACTIVE_HIGH
    #define NPMX_ESP32_INT_INTR_TYPE        GPIO_INTR_HIGH_LEVEL
#else
    #define NPMX_ESP32_INT_INTR_TYPE        GPIO_INTR_LOW_LEVEL
#endif

/**
 * @brief 移植层运行上下文状态结构体
 */
typedef struct {
    bool                    ready;          /**< 驱动是否就绪 */
    bool                    owns_bus;       /**< 是否自主创建了 I2C 总线（deinit 时需负责销毁） */
    bool                    int_isr_added;  /**< INT 中断处理函数是否已注册 */
    bool                    pof_isr_added;  /**< POF 中断处理函数是否已注册 */
    i2c_master_bus_handle_t bus;            /**< I2C 主机总线句柄 */
    i2c_master_dev_handle_t dev;            /**< I2C 从机设备句柄 */
    npmx_instance_t         instance;       /**< npmx 官方核心实例 */
    npmx_backend_t          backend;        /**< npmx 通信后端接口 */
    SemaphoreHandle_t       lock;           /**< 递归互斥锁，保护多任务并发访问与中断打断 */
    TaskHandle_t            evt_task;       /**< 中断下半部处理任务句柄 */
    npmx_esp32_pof_cb_t     pof_cb;         /**< 用户注册的 POF 掉电回调函数 */
} npmx_esp32_ctx_t;

static npmx_esp32_ctx_t s_ctx;

/* --------------------------------------------------------------------------
 * 1. 递归互斥锁管理（防重入、防死锁）
 * -------------------------------------------------------------------------- */

void npmx_esp32_lock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreTakeRecursive(s_ctx.lock, portMAX_DELAY);
    }
}

void npmx_esp32_unlock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreGiveRecursive(s_ctx.lock);
    }
}

/* --------------------------------------------------------------------------
 * 2. npmx 硬件抽象后端（I2C 读写适配）
 * -------------------------------------------------------------------------- */

static npmx_error_t npmx_esp32_i2c_write(void *p_context, uint32_t register_address,
                                         uint8_t *p_data, size_t num_of_bytes)
{
    npmx_esp32_ctx_t *ctx = (npmx_esp32_ctx_t *)p_context;
    uint8_t buf[2 + NPMX_ESP32_MAX_XFER];

    if ((ctx == NULL) || (ctx->dev == NULL) || (p_data == NULL) ||
        (num_of_bytes == 0U) || (num_of_bytes > NPMX_ESP32_MAX_XFER)) {
        return NPMX_ERROR_INVALID_PARAM;
    }

    /* nPM1300/nPM1304 寄存器地址为 16 位大端序 (MSB first) */
    buf[0] = (uint8_t)(register_address >> 8);
    buf[1] = (uint8_t)(register_address & 0xFFU);
    memcpy(&buf[2], p_data, num_of_bytes);

    npmx_esp32_lock();
    /* ESP-IDF v5 i2c_master_transmit 超时参数为毫秒数 (ms)，不可转换为 tick */
    esp_err_t err = i2c_master_transmit(ctx->dev, buf, num_of_bytes + 2U,
                                        NPMX_ESP32_I2C_TIMEOUT_MS);
    npmx_esp32_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 写入失败 [寄存器 0x%04" PRIX32 ", 长度 %zu 字节]: %s",
                 register_address, num_of_bytes, esp_err_to_name(err));
        return NPMX_ERROR_IO;
    }

    return NPMX_SUCCESS;
}

static npmx_error_t npmx_esp32_i2c_read(void *p_context, uint32_t register_address,
                                        uint8_t *p_data, size_t num_of_bytes)
{
    npmx_esp32_ctx_t *ctx = (npmx_esp32_ctx_t *)p_context;
    uint8_t reg_addr[2];

    if ((ctx == NULL) || (ctx->dev == NULL) || (p_data == NULL) ||
        (num_of_bytes == 0U) || (num_of_bytes > NPMX_ESP32_MAX_XFER)) {
        return NPMX_ERROR_INVALID_PARAM;
    }

    reg_addr[0] = (uint8_t)(register_address >> 8);
    reg_addr[1] = (uint8_t)(register_address & 0xFFU);

    npmx_esp32_lock();
    /* ESP-IDF v5 i2c_master_transmit_receive 超时参数为毫秒数 (ms) */
    esp_err_t err = i2c_master_transmit_receive(ctx->dev, reg_addr, sizeof(reg_addr),
                                                p_data, num_of_bytes,
                                                NPMX_ESP32_I2C_TIMEOUT_MS);
    npmx_esp32_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 读取失败 [寄存器 0x%04" PRIX32 ", 长度 %zu 字节]: %s",
                 register_address, num_of_bytes, esp_err_to_name(err));
        return NPMX_ERROR_IO;
    }

    return NPMX_SUCCESS;
}

/* --------------------------------------------------------------------------
 * 3. 默认通用事件回调
 * -------------------------------------------------------------------------- */

static void npmx_esp32_generic_cb(npmx_instance_t *p_pm, npmx_callback_type_t type, uint8_t mask)
{
    (void)p_pm;

    ESP_LOGD(TAG, "收到 PMIC 事件: %s (掩码 0x%02X)", npmx_callback_to_str(type), mask);
    for (uint8_t i = 0; i < 8; i++) {
        if (mask & (1U << i)) {
            ESP_LOGD(TAG, "\t- %s", npmx_callback_bit_to_str(type, i));
        }
    }
}

/* --------------------------------------------------------------------------
 * 4. FreeRTOS 中断与下半部事件任务（基于 Task Notification）
 * -------------------------------------------------------------------------- */

#if (NPMX_ESP32_HAS_INT || NPMX_ESP32_HAS_POF)

static void IRAM_ATTR npmx_esp32_gpio_isr(void *arg)
{
    uint32_t notif_bit = (uint32_t)arg;
    BaseType_t woken = pdFALSE;

#if NPMX_ESP32_HAS_INT
    if (notif_bit == NPMX_ESP32_NOTIF_INT) {
        /* 电平触发中断：在退出 ISR 前先禁用中断，防止中断风暴 */
        gpio_intr_disable(NPMX_ESP32_INT_GPIO);
    }
#endif

#if NPMX_ESP32_HAS_POF
    if (notif_bit == NPMX_ESP32_NOTIF_POF) {
        /* 掉电跌落时同时关闭两个中断源 */
        gpio_intr_disable(NPMX_ESP32_POF_GPIO);
#if NPMX_ESP32_HAS_INT
        gpio_intr_disable(NPMX_ESP32_INT_GPIO);
#endif
    }
#endif

    if (s_ctx.evt_task != NULL) {
        xTaskNotifyFromISR(s_ctx.evt_task, notif_bit, eSetBits, &woken);
    }

    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void npmx_esp32_event_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t notif_bits = 0;

        if (xTaskNotifyWait(0U, UINT32_MAX, &notif_bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }

#if NPMX_ESP32_HAS_POF
        if (notif_bits & NPMX_ESP32_NOTIF_POF) {
            ESP_LOGW(TAG, "捕获到 POF 掉电告警！");
            npmx_esp32_lock();
            if (s_ctx.pof_cb != NULL) {
                s_ctx.pof_cb(&s_ctx.instance);
            }
            npmx_esp32_unlock();
        }
#endif

#if NPMX_ESP32_HAS_INT
        if (notif_bits & NPMX_ESP32_NOTIF_INT) {
            npmx_esp32_lock();
            npmx_core_interrupt(&s_ctx.instance);
            npmx_error_t err = npmx_core_proc(&s_ctx.instance);
            npmx_esp32_unlock();

            if (err != NPMX_SUCCESS) {
                ESP_LOGW(TAG, "PMIC 事件处理失败: %s", npmx_esp32_error_to_str(err));
            }

            /* 清理并处理完事件后，重新开启主机 GPIO 中断接收 */
            if (s_ctx.int_isr_added) {
                gpio_intr_enable(NPMX_ESP32_INT_GPIO);
            }
        }
#endif
    }
}

static esp_err_t npmx_esp32_isr_service_ensure(void)
{
    esp_err_t err = gpio_install_isr_service(0);

    /* 若已安装则直接复用，不视作错误 */
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t npmx_esp32_event_task_start(void)
{
    BaseType_t ret = xTaskCreate(npmx_esp32_event_task, "npmx_evt",
                                 NPMX_ESP32_TASK_STACK_SIZE, NULL,
                                 NPMX_ESP32_TASK_PRIORITY, &s_ctx.evt_task);
    if (ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#endif /* NPMX_ESP32_HAS_INT || NPMX_ESP32_HAS_POF */

#if NPMX_ESP32_HAS_INT
static esp_err_t npmx_esp32_int_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NPMX_ESP32_INT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = NPMX_ESP32_INT_ACTIVE_HIGH ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = NPMX_ESP32_INT_ACTIVE_HIGH ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = NPMX_ESP32_INT_INTR_TYPE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = npmx_esp32_isr_service_ensure();
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_isr_handler_add(NPMX_ESP32_INT_GPIO, npmx_esp32_gpio_isr,
                               (void *)NPMX_ESP32_NOTIF_INT);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.int_isr_added = true;

    return gpio_intr_enable(NPMX_ESP32_INT_GPIO);
}
#endif /* NPMX_ESP32_HAS_INT */

/* --------------------------------------------------------------------------
 * 5. 资源释放与总线创建
 * -------------------------------------------------------------------------- */

static void npmx_esp32_resources_free(void)
{
#if NPMX_ESP32_HAS_INT
    if (s_ctx.int_isr_added) {
        gpio_intr_disable(NPMX_ESP32_INT_GPIO);
        gpio_isr_handler_remove(NPMX_ESP32_INT_GPIO);
        s_ctx.int_isr_added = false;
    }
#endif

#if NPMX_ESP32_HAS_POF
    if (s_ctx.pof_isr_added) {
        gpio_intr_disable(NPMX_ESP32_POF_GPIO);
        gpio_isr_handler_remove(NPMX_ESP32_POF_GPIO);
        s_ctx.pof_isr_added = false;
    }
#endif

    if (s_ctx.lock != NULL) {
        (void)xSemaphoreTakeRecursive(s_ctx.lock, portMAX_DELAY);
    }

#if (NPMX_ESP32_HAS_INT || NPMX_ESP32_HAS_POF)
    if (s_ctx.evt_task != NULL) {
        vTaskDelete(s_ctx.evt_task);
        s_ctx.evt_task = NULL;
    }
#endif

    if (s_ctx.dev != NULL) {
        i2c_master_bus_rm_device(s_ctx.dev);
        s_ctx.dev = NULL;
    }

    if (s_ctx.owns_bus && (s_ctx.bus != NULL)) {
        i2c_del_master_bus(s_ctx.bus);
    }
    s_ctx.bus = NULL;
    s_ctx.owns_bus = false;

    if (s_ctx.lock != NULL) {
        xSemaphoreGiveRecursive(s_ctx.lock);
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }

    s_ctx.pof_cb = NULL;
    s_ctx.ready = false;
}

static esp_err_t npmx_esp32_bus_create(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = NPMX_ESP32_I2C_PORT,
        .scl_io_num = NPMX_ESP32_I2C_SCL_GPIO,
        .sda_io_num = NPMX_ESP32_I2C_SDA_GPIO,
        .glitch_ignore_cnt = NPMX_ESP32_I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = (NPMX_ESP32_I2C_INTERNAL_PULLUP != 0),
    };

    return i2c_new_master_bus(&bus_cfg, &s_ctx.bus);
}

/* --------------------------------------------------------------------------
 * 6. 主初始化与控制 API
 * -------------------------------------------------------------------------- */

esp_err_t npmx_esp32_init(i2c_master_bus_handle_t bus)
{
    if (s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    npmx_error_t nerr;

    memset(&s_ctx, 0, sizeof(s_ctx));

    /* 创建递归互斥锁 */
    s_ctx.lock = xSemaphoreCreateRecursiveMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 配置或新建 I2C 主机总线 */
    if (bus != NULL) {
        s_ctx.bus = bus;
        s_ctx.owns_bus = false;
    } else {
        err = npmx_esp32_bus_create();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "创建 I2C 主机总线失败: %s", esp_err_to_name(err));
            npmx_esp32_resources_free();
            return err;
        }
        s_ctx.owns_bus = true;
    }

    /* 挂载 PMIC I2C 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NPMX_ESP32_I2C_ADDR,
        .scl_speed_hz = NPMX_ESP32_I2C_FREQ_HZ,
    };

    err = i2c_master_bus_add_device(s_ctx.bus, &dev_cfg, &s_ctx.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "向 I2C 总线挂载 PMIC 设备失败: %s", esp_err_to_name(err));
        npmx_esp32_resources_free();
        return err;
    }

    /* 配置通信后端接口 */
    s_ctx.backend.p_write = npmx_esp32_i2c_write;
    s_ctx.backend.p_read = npmx_esp32_i2c_read;
    s_ctx.backend.p_context = &s_ctx;

    /* 初始化 npmx 官方核心实例 */
    nerr = npmx_core_init(&s_ctx.instance, &s_ctx.backend, npmx_esp32_generic_cb,
                          (NPMX_ESP32_RESTORE_VALUES != 0));
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "npmx_core_init 核心初始化失败: %s", npmx_esp32_error_to_str(nerr));
        npmx_esp32_resources_free();
        return ESP_FAIL;
    }

    /* 验证芯片是否真实在线并读取硬件版本号 */
    uint8_t major = 0, minor = 0, patch = 0;
    nerr = npmx_core_pmic_revision_get(&s_ctx.instance, &major, &minor, &patch);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "从机地址 0x%02X 无响应，未探测到 PMIC 芯片！请检查硬件接线 (%s)",
                 (unsigned int)NPMX_ESP32_I2C_ADDR, npmx_esp32_error_to_str(nerr));
        npmx_esp32_resources_free();
        return ESP_ERR_NOT_FOUND;
    }

#if (NPMX_ESP32_HAS_INT || NPMX_ESP32_HAS_POF)
    /* 启动 FreeRTOS 中断事件下半部任务 */
    err = npmx_esp32_event_task_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动事件下半部任务失败: %s", esp_err_to_name(err));
        npmx_esp32_resources_free();
        return err;
    }
#endif

    /* 开启任何中断源之前，关闭并清除 PMIC 所有挂起事件 */
    for (uint32_t i = 0; i < NPMX_EVENT_GROUP_COUNT; i++) {
        nerr = npmx_core_event_interrupt_disable(&s_ctx.instance, (npmx_event_group_t)i,
                                                 NPMX_EVENT_GROUP_ALL_EVENTS_MASK);
        if (nerr != NPMX_SUCCESS) {
            ESP_LOGW(TAG, "清除事件组 %" PRIu32 " 失败: %s", i,
                     npmx_esp32_error_to_str(nerr));
        }
    }

#if NPMX_ESP32_HAS_INT
    if (NPMX_ESP32_PMIC_INT_PIN < 0) {
        ESP_LOGE(TAG, "PMIC INT 引脚编号未指定！");
        npmx_esp32_resources_free();
        return ESP_ERR_INVALID_ARG;
    }

    /* 配置 PMIC 的 GPIO 引脚为 IRQ 中断输出模式 */
    nerr = npmx_gpio_mode_set(npmx_gpio_get(&s_ctx.instance, NPMX_ESP32_PMIC_INT_PIN),
                              NPMX_GPIO_MODE_OUTPUT_IRQ);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "配置 PMIC INT 引脚模式失败: %s", npmx_esp32_error_to_str(nerr));
        npmx_esp32_resources_free();
        return ESP_FAIL;
    }

    /* 配置 ESP32 侧输入 GPIO 及中断 ISR */
    err = npmx_esp32_int_gpio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 ESP32 INT GPIO 失败: %s", esp_err_to_name(err));
        npmx_esp32_resources_free();
        return err;
    }
#endif

#if (NPMX_ESP32_PMIC_RESET_PIN >= 0)
    /* 若配置了 PMIC 复位引脚，将其配置为复位输出 */
    nerr = npmx_gpio_mode_set(npmx_gpio_get(&s_ctx.instance, NPMX_ESP32_PMIC_RESET_PIN),
                              NPMX_GPIO_MODE_OUTPUT_RESET);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGW(TAG, "配置 PMIC RESET 引脚失败: %s", npmx_esp32_error_to_str(nerr));
    }
#endif

    s_ctx.ready = true;

    ESP_LOGI(TAG, "PMIC nPM13xx 移植层初始化成功！硬件版本: v%u.%u.%u", major, minor, patch);

    return ESP_OK;
}

esp_err_t npmx_esp32_init_default(void)
{
    return npmx_esp32_init(NULL);
}

esp_err_t npmx_esp32_deinit(void)
{
    if (!s_ctx.ready && (s_ctx.lock == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    npmx_esp32_resources_free();
    return ESP_OK;
}

bool npmx_esp32_is_ready(void)
{
    return s_ctx.ready;
}

npmx_instance_t *npmx_esp32_instance_get(void)
{
    return s_ctx.ready ? &s_ctx.instance : NULL;
}

i2c_master_bus_handle_t npmx_esp32_i2c_bus_get(void)
{
    return s_ctx.bus;
}

i2c_master_dev_handle_t npmx_esp32_i2c_dev_get(void)
{
    return s_ctx.dev;
}

/* --------------------------------------------------------------------------
 * 7. 事件回调与中断使能 API
 * -------------------------------------------------------------------------- */

esp_err_t npmx_esp32_register_cb(npmx_callback_type_t type, npmx_callback_t cb)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (type >= NPMX_CALLBACK_TYPE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    npmx_core_register_cb(&s_ctx.instance, cb, type);
    return ESP_OK;
}

esp_err_t npmx_esp32_event_interrupt_enable(npmx_event_group_t group, uint8_t mask)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
#if !NPMX_ESP32_HAS_INT
    return ESP_ERR_NOT_SUPPORTED;
#else
    npmx_error_t nerr = npmx_core_event_interrupt_enable(&s_ctx.instance, group, mask);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "使能事件组 %u 中断失败: %s", (unsigned int)group,
                 npmx_esp32_error_to_str(nerr));
        return ESP_FAIL;
    }
    return ESP_OK;
#endif
}

esp_err_t npmx_esp32_event_interrupt_disable(npmx_event_group_t group, uint8_t mask)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    npmx_error_t nerr = npmx_core_event_interrupt_disable(&s_ctx.instance, group, mask);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "禁用事件组 %u 中断失败: %s", (unsigned int)group,
                 npmx_esp32_error_to_str(nerr));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * 8. POF（掉电预警）相关 API
 * -------------------------------------------------------------------------- */

#if NPMX_ESP32_HAS_POF
esp_err_t npmx_esp32_register_pof_cb(const npmx_pof_config_t *config, npmx_esp32_pof_cb_t cb)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((config == NULL) || (cb == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (NPMX_ESP32_PMIC_POF_PIN < 0) {
        ESP_LOGE(TAG, "PMIC POF 引脚编号未指定！");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = npmx_esp32_isr_service_ensure();
    if (err != ESP_OK) {
        return err;
    }

    /* 将 PMIC GPIO 配置为 PLW (Power Loss Warning) 输出模式 */
    npmx_error_t nerr = npmx_gpio_mode_set(npmx_gpio_get(&s_ctx.instance, NPMX_ESP32_PMIC_POF_PIN),
                                           NPMX_GPIO_MODE_OUTPUT_PLW);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "配置 PMIC POF 引脚模式失败: %s", npmx_esp32_error_to_str(nerr));
        return ESP_FAIL;
    }

    nerr = npmx_pof_config_set(npmx_pof_get(&s_ctx.instance, 0), config);
    if (nerr != NPMX_SUCCESS) {
        ESP_LOGE(TAG, "配置 PMIC POF 模块失败: %s", npmx_esp32_error_to_str(nerr));
        return ESP_FAIL;
    }

    bool active_high = (config->polarity == NPMX_POF_POLARITY_HIGH);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NPMX_ESP32_POF_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = active_high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL,
    };

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_isr_handler_add(NPMX_ESP32_POF_GPIO, npmx_esp32_gpio_isr,
                               (void *)NPMX_ESP32_NOTIF_POF);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.pof_isr_added = true;
    s_ctx.pof_cb = cb;

    return gpio_intr_enable(NPMX_ESP32_POF_GPIO);
}

esp_err_t npmx_esp32_pof_rearm(void)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.pof_isr_added) {
        gpio_intr_enable(NPMX_ESP32_POF_GPIO);
    }

#if NPMX_ESP32_HAS_INT
    if (s_ctx.int_isr_added) {
        gpio_intr_enable(NPMX_ESP32_INT_GPIO);
    }
#endif

    return ESP_OK;
}
#else  /* NPMX_ESP32_HAS_POF */
esp_err_t npmx_esp32_register_pof_cb(const npmx_pof_config_t *config, npmx_esp32_pof_cb_t cb)
{
    (void)config;
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t npmx_esp32_pof_rearm(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif /* NPMX_ESP32_HAS_POF */

/* --------------------------------------------------------------------------
 * 9. 错误码中文可读转换
 * -------------------------------------------------------------------------- */

const char *npmx_esp32_error_to_str(npmx_error_t err)
{
    switch (err) {
    case NPMX_SUCCESS:
        return "成功 (NPMX_SUCCESS)";
    case NPMX_ERROR_INVALID_PARAM:
        return "参数无效 (NPMX_ERROR_INVALID_PARAM)";
    case NPMX_ERROR_IO:
        return "底层通信错误 (NPMX_ERROR_IO)";
    case NPMX_ERROR_INVALID_MEAS:
        return "采样数据未就绪或无效 (NPMX_ERROR_INVALID_MEAS)";
    case NPMX_ERROR_NOT_SUPPORTED:
        return "功能不支持 (NPMX_ERROR_NOT_SUPPORTED)";
    default:
        return "未知错误 (NPMX_ERROR_UNKNOWN)";
    }
}

/* --------------------------------------------------------------------------
 * 10. 便捷辅助函数（ADC 采样与信息打印）
 * -------------------------------------------------------------------------- */

static npmx_error_t npmx_adc_sample_helper(npmx_adc_task_t task, npmx_adc_meas_t meas,
                                           int32_t *p_value)
{
    if (!s_ctx.ready) {
        return NPMX_ERROR_IO;
    }

    npmx_adc_t *adc = npmx_adc_get(&s_ctx.instance, 0);
    if (adc == NULL) {
        return NPMX_ERROR_INVALID_PARAM;
    }

    npmx_esp32_lock();
    npmx_error_t err = npmx_adc_task_trigger(adc, task);
    npmx_esp32_unlock();

    if (err != NPMX_SUCCESS) {
        return err;
    }

    /* 轮询等待 ADC 转换就绪，最多等待 200 毫秒 */
    for (int retry = 0; retry < 20; retry++) {
        bool ready = false;
        npmx_esp32_lock();
        err = npmx_adc_meas_check(adc, meas, &ready);
        npmx_esp32_unlock();

        if (err != NPMX_SUCCESS) {
            return err;
        }

        if (ready) {
            npmx_esp32_lock();
            err = npmx_adc_meas_get(adc, meas, p_value);
            npmx_esp32_unlock();
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return NPMX_ERROR_INVALID_MEAS;
}

npmx_error_t npmx_esp32_vbat_get_mv(int32_t *p_vbat_mv)
{
    return npmx_adc_sample_helper(NPMX_ADC_TASK_SINGLE_SHOT_VBAT, NPMX_ADC_MEAS_VBAT, p_vbat_mv);
}

npmx_error_t npmx_esp32_vsys_get_mv(int32_t *p_vsys_mv)
{
    return npmx_adc_sample_helper(NPMX_ADC_TASK_SINGLE_SHOT_VSYS, NPMX_ADC_MEAS_VSYS, p_vsys_mv);
}

npmx_error_t npmx_esp32_die_temp_get_c(float *p_temp_c)
{
    int32_t val_mdeg = 0;
    npmx_error_t err = npmx_adc_sample_helper(NPMX_ADC_TASK_SINGLE_SHOT_DIE_TEMP,
                                             NPMX_ADC_MEAS_DIE_TEMP, &val_mdeg);
    if ((err == NPMX_SUCCESS) && (p_temp_c != NULL)) {
        *p_temp_c = (float)val_mdeg / 1000.0f;
    }
    return err;
}

void npmx_esp32_dump_status(void)
{
    if (!s_ctx.ready) {
        ESP_LOGW(TAG, "PMIC 驱动未初始化，无法打印状态！");
        return;
    }

    uint8_t major = 0, minor = 0, patch = 0;
    npmx_core_pmic_revision_get(&s_ctx.instance, &major, &minor, &patch);

    ESP_LOGI(TAG, "==================== PMIC 状态信息概览 ====================");
    ESP_LOGI(TAG, "芯片固件版本 : v%u.%u.%u", major, minor, patch);

    /* 1. BUCK 输出状态 */
    for (uint8_t i = 0; i < 2; i++) {
        npmx_buck_t *buck = npmx_buck_get(&s_ctx.instance, i);
        if (buck) {
            npmx_buck_voltage_t norm_v;
            uint32_t norm_mv = 0;
            if (npmx_buck_normal_voltage_get(buck, &norm_v) == NPMX_SUCCESS) {
                npmx_buck_voltage_convert_to_mv(norm_v, &norm_mv);
                ESP_LOGI(TAG, "BUCK%u 目标电压: %" PRIu32 " mV", i + 1, norm_mv);
            }
        }
    }

    /* 2. 充电器状态 */
    npmx_charger_t *charger = npmx_charger_get(&s_ctx.instance, 0);
    if (charger) {
        uint8_t chg_status = 0;
        if (npmx_charger_status_get(charger, &chg_status) == NPMX_SUCCESS) {
            ESP_LOGI(TAG, "充电器状态码: 0x%02X [%s%s%s%s]", chg_status,
                     (chg_status & NPMX_CHARGER_STATUS_BATTERY_DETECTED_MASK) ? "电池已接入 " : "无电池 ",
                     (chg_status & NPMX_CHARGER_STATUS_TRICKLE_CHARGE_MASK)   ? "涓流充电中 " : "",
                     (chg_status & NPMX_CHARGER_STATUS_CONSTANT_CURRENT_MASK) ? "恒流充电中 " : "",
                     (chg_status & NPMX_CHARGER_STATUS_COMPLETED_MASK)        ? "充电已完成" : "空闲");
        }
    }

    /* 3. VBUS 输入状态 */
    npmx_vbusin_t *vbus = npmx_vbusin_get(&s_ctx.instance, 0);
    if (vbus) {
        uint8_t vbus_status = 0;
        if (npmx_vbusin_vbus_status_get(vbus, &vbus_status) == NPMX_SUCCESS) {
            ESP_LOGI(TAG, "VBUS 输入状态: 0x%02X (%s)", vbus_status,
                     (vbus_status != 0) ? "已连接外部电源" : "未连接外部电源");
        }
    }
    ESP_LOGI(TAG, "==========================================================");
}
