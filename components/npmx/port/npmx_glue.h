/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file npmx_glue.h
 * @brief npmx 官方驱动库在 ESP-IDF (FreeRTOS) 环境下的胶水适配层
 */

#ifndef NPMX_GLUE_H__
#define NPMX_GLUE_H__

#include <assert.h>
#include <npmx.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup npmx_glue npmx_glue.h
 * @{
 * @ingroup npmx
 *
 * @brief npmx 官方库集成至 ESP-IDF 平台所需的断言与抽象宏
 */

/**
 * @brief 运行时断言宏
 */
#define NPMX_ASSERT(expression) assert(expression)

/**
 * @brief 编译期静态断言宏（兼容 C++、C11 以及传统 C99 编译器）
 */
#if defined(__cplusplus)
    #define NPMX_STATIC_ASSERT(expression) static_assert(expression, #expression)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    #define NPMX_STATIC_ASSERT(expression) _Static_assert(expression, #expression)
#else
    #define NPMX_STATIC_ASSERT(expression) typedef char __npmx_static_assert__[(expression) ? 1 : -1]
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* NPMX_GLUE_H__ */
