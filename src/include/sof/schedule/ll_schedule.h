/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

/*
 * Delayed or scheduled work. Work runs in the same context as it's timer
 * interrupt source. It should execute quickly and must not sleep or wait.
 */

#ifndef __SOF_SCHEDULE_LL_SCHEDULE_H__
#define __SOF_SCHEDULE_LL_SCHEDULE_H__

#include <sof/schedule/task.h>
#include <sof/trace/trace.h>
#include <user/trace.h>

struct ll_schedule_domain;

/* ll tracing */
#define trace_ll(format, ...) \
	trace_event(TRACE_CLASS_SCHEDULE_LL, format, ##__VA_ARGS__)

#define trace_ll_error(format, ...) \
	trace_error(TRACE_CLASS_SCHEDULE_LL, format, ##__VA_ARGS__)

#define tracev_ll(format, ...) \
	tracev_event(TRACE_CLASS_SCHEDULE_LL, format, ##__VA_ARGS__)

int scheduler_init_ll(struct ll_schedule_domain *domain);

#endif /* __SOF_SCHEDULE_LL_SCHEDULE_H__ */
