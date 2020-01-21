/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

/**
 * \file include/sof/lib/cpu.h
 * \brief CPU header file
 * \authors Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

#ifndef __SOF_LIB_CPU_H__
#define __SOF_LIB_CPU_H__

#include <platform/lib/cpu.h>

#if !defined(__ASSEMBLER__) && !defined(LINKER)

#include <arch/lib/cpu.h>
#include <stdbool.h>

#if PLATFORM_CORE_COUNT > MAX_CORE_COUNT
#error "Invalid core count - exceeding core limit"
#endif

static inline int cpu_get_id(void)
{
	return arch_cpu_get_id();
}

static inline bool cpu_is_slave(int id)
{
	return id != PLATFORM_MASTER_CORE_ID;
}

static inline bool cpu_is_me(int id)
{
	return id == cpu_get_id();
}

static inline void cpu_enable_core(int id)
{
	arch_cpu_enable_core(id);
}

static inline void cpu_disable_core(int id)
{
	arch_cpu_disable_core(id);
}

static inline int cpu_is_core_enabled(int id)
{
	return arch_cpu_is_core_enabled(id);
}

#endif

#endif /* __SOF_LIB_CPU_H__ */
