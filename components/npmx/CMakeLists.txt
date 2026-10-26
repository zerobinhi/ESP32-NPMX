#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: BSD-3-Clause
#
# npmx PMIC ESP-IDF 组件 CMake 规则
#
# 说明：本组件不使用任何 Kconfig 选项，所有硬件及驱动参数均通过
#       include/npmx_esp32_config.h 中的 #define 进行配置。
#

set(npmx_dir "${CMAKE_CURRENT_LIST_DIR}/npmx")

set(npmx_srcs
    "npmx_esp32.c"
    "${npmx_dir}/backends/npmx_backend.c"
    "${npmx_dir}/drivers/src/npmx_adc.c"
    "${npmx_dir}/drivers/src/npmx_buck.c"
    "${npmx_dir}/drivers/src/npmx_charger.c"
    "${npmx_dir}/drivers/src/npmx_common.c"
    "${npmx_dir}/drivers/src/npmx_core.c"
    "${npmx_dir}/drivers/src/npmx_errlog.c"
    "${npmx_dir}/drivers/src/npmx_gpio.c"
    "${npmx_dir}/drivers/src/npmx_ldsw.c"
    "${npmx_dir}/drivers/src/npmx_led.c"
    "${npmx_dir}/drivers/src/npmx_pof.c"
    "${npmx_dir}/drivers/src/npmx_ship.c"
    "${npmx_dir}/drivers/src/npmx_timer.c"
    "${npmx_dir}/drivers/src/npmx_vbusin.c"
)

idf_component_register(
    SRCS ${npmx_srcs}
    INCLUDE_DIRS
        "include"
        "port"
        "${npmx_dir}"
        "${npmx_dir}/backends"
        "${npmx_dir}/drivers/include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        driver
        freertos
        log
)

# npmx 官方驱动库在部分函数声明中使用了 const 修饰的基础类型返回值，
# 增加 -Wno-ignored-qualifiers 编译选项以防止在 -Werror 规则下被中断。
target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-ignored-qualifiers)
