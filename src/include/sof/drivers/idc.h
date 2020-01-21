/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

/**
 * \file include/sof/drivers/idc.h
 * \brief IDC header file
 * \authors Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

#ifndef __SOF_DRIVERS_IDC_H__
#define __SOF_DRIVERS_IDC_H__

#include <platform/drivers/idc.h>
#include <sof/lib/cpu.h>
#include <sof/schedule/task.h>
#include <sof/trace/trace.h>
#include <user/trace.h>
#include <stdbool.h>
#include <stdint.h>

/** \brief IDC trace function. */
#define trace_idc(__e, ...) \
	trace_event(TRACE_CLASS_IDC, __e, ##__VA_ARGS__)

/** \brief IDC trace value function. */
#define tracev_idc(__e, ...) \
	tracev_event(TRACE_CLASS_IDC, __e, ##__VA_ARGS__)

/** \brief IDC trace error function. */
#define trace_idc_error(__e, ...) \
	trace_error(TRACE_CLASS_IDC, __e, ##__VA_ARGS__)

/** \brief IDC send blocking flag. */
#define IDC_BLOCKING		0

/** \brief IDC send non-blocking flag. */
#define IDC_NON_BLOCKING	1

/** \brief IDC send timeout in microseconds. */
#define IDC_TIMEOUT	10000

/** \brief IDC task deadline. */
#define IDC_DEADLINE	100

/** \brief ROM wake version parsed by ROM during core wake up. */
#define IDC_ROM_WAKE_VERSION	0x2

/** \brief IDC message type. */
#define IDC_TYPE_SHIFT		24
#define IDC_TYPE_MASK		0x7f
#define IDC_TYPE(x)		(((x) & IDC_TYPE_MASK) << IDC_TYPE_SHIFT)

/** \brief IDC message header. */
#define IDC_HEADER_MASK		0xffffff
#define IDC_HEADER(x)		((x) & IDC_HEADER_MASK)

/** \brief IDC message extension. */
#define IDC_EXTENSION_MASK	0x3fffffff
#define IDC_EXTENSION(x)	((x) & IDC_EXTENSION_MASK)

/** \brief IDC power up message. */
#define IDC_MSG_POWER_UP	(IDC_TYPE(0x1) | \
					IDC_HEADER(IDC_ROM_WAKE_VERSION))
#define IDC_MSG_POWER_UP_EXT	IDC_EXTENSION(SOF_TEXT_START >> 2)

/** \brief IDC power down message. */
#define IDC_MSG_POWER_DOWN	IDC_TYPE(0x2)
#define IDC_MSG_POWER_DOWN_EXT	IDC_EXTENSION(0x0)

/** \brief IDC notify message. */
#define IDC_MSG_NOTIFY		IDC_TYPE(0x3)
#define IDC_MSG_NOTIFY_EXT	IDC_EXTENSION(0x0)

/** \brief IDC IPC processing message. */
#define IDC_MSG_IPC		IDC_TYPE(0x4)
#define IDC_MSG_IPC_EXT		IDC_EXTENSION(0x0)

/** \brief Decodes IDC message type. */
#define iTS(x)	(((x) >> IDC_TYPE_SHIFT) & IDC_TYPE_MASK)

/** \brief IDC message. */
struct idc_msg {
	uint32_t header;	/**< header value */
	uint32_t extension;	/**< extension value */
	uint32_t core;		/**< core id */
};

/** \brief IDC data. */
struct idc {
	uint32_t busy_bit_mask;		/**< busy interrupt mask */
	uint32_t done_bit_mask;		/**< done interrupt mask */
	struct idc_msg received_msg;	/**< received message */
	struct task idc_task;		/**< IDC processing task */
	bool msg_processed[PLATFORM_CORE_COUNT];
	int irq;
};

void idc_enable_interrupts(int target_core, int source_core);

void idc_free(void);

#endif /* __SOF_DRIVERS_IDC_H__ */
