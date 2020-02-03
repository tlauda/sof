// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>

#include <sof/audio/component.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/drivers/interrupt.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/list.h>
#include <sof/schedule/ll_schedule.h>
#include <sof/schedule/schedule.h>
#include <sof/sof.h>
#include <sof/string.h>
#include <ipc/topology.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static SHARED_DATA struct comp_driver_list cd;

static const struct comp_driver *get_drv(uint32_t type)
{
	struct comp_driver_list *drivers = comp_drivers_get();
	struct list_item *clist;
	const struct comp_driver *drv = NULL;
	struct comp_driver_info *info;
	uint32_t flags;

	irq_local_disable(flags);

	/* search driver list for driver type */
	list_for_item(clist, &drivers->list) {
		info = container_of(clist, struct comp_driver_info, list);
		if (info->drv->type == type) {
			drv = info->drv;
			platform_shared_commit(info, sizeof(*info));
			goto out;
		}

		platform_shared_commit(info, sizeof(*info));
	}

out:
	platform_shared_commit(drivers, sizeof(*drivers));
	irq_local_enable(flags);
	return drv;
}

struct comp_dev *comp_new(struct sof_ipc_comp *comp)
{
	struct comp_dev *cdev;
	const struct comp_driver *drv;
	int ret;

	/* find the driver for our new component */
	drv = get_drv(comp->type);
	if (!drv) {
		trace_comp_error("comp_new() error: driver not found, "
				 "comp->type = %u", comp->type);
		return NULL;
	}

	/* create the new component */
	cdev = drv->ops.new(comp);
	if (!cdev) {
		trace_comp_error("comp_new() error: "
				 "unable to create the new component");
		return NULL;
	}

	/* init component */
	ret = memcpy_s(&cdev->comp, sizeof(cdev->comp),
		       comp, sizeof(*comp));
	assert(!ret);

	cdev->drv = drv;
	list_init(&cdev->bsource_list);
	list_init(&cdev->bsink_list);

	return cdev;
}

int comp_register(struct comp_driver_info *drv)
{
	struct comp_driver_list *drivers = comp_drivers_get();
	uint32_t flags;

	irq_local_disable(flags);
	list_item_prepend(&drv->list, &drivers->list);
	platform_shared_commit(drv, sizeof(*drv));
	platform_shared_commit(drivers, sizeof(*drivers));
	irq_local_enable(flags);

	return 0;
}

void comp_unregister(struct comp_driver_info *drv)
{
	uint32_t flags;

	irq_local_disable(flags);
	list_item_del(&drv->list);
	platform_shared_commit(drv, sizeof(*drv));
	irq_local_enable(flags);
}

int comp_set_state(struct comp_dev *dev, int cmd)
{
	int requested_state = comp_get_requested_state(cmd);
	int ret = 0;

	if (dev->state == requested_state) {
		trace_comp_with_ids(dev, "comp_set_state(), "
				    "state already set to %u", dev->state);
		return COMP_STATUS_STATE_ALREADY_SET;
	}

	switch (cmd) {
	case COMP_TRIGGER_START:
		if (dev->state == COMP_STATE_PREPARE) {
			dev->state = COMP_STATE_ACTIVE;
		} else {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_START",
						  dev->state);
			ret = -EINVAL;
		}
		break;
	case COMP_TRIGGER_RELEASE:
		if (dev->state == COMP_STATE_PAUSED) {
			dev->state = COMP_STATE_ACTIVE;
		} else {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_RELEASE",
						  dev->state);
			ret = -EINVAL;
		}
		break;
	case COMP_TRIGGER_STOP:
		if (dev->state == COMP_STATE_ACTIVE ||
		    dev->state == COMP_STATE_PAUSED) {
			dev->state = COMP_STATE_PREPARE;
		} else {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_STOP",
						  dev->state);
			ret = -EINVAL;
		}
		break;
	case COMP_TRIGGER_XRUN:
		/* reset component status to ready at xrun */
		dev->state = COMP_STATE_READY;
		break;
	case COMP_TRIGGER_PAUSE:
		/* only support pausing for running */
		if (dev->state == COMP_STATE_ACTIVE) {
			dev->state = COMP_STATE_PAUSED;
		} else {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_PAUSE",
						  dev->state);
			ret = -EINVAL;
		}
		break;
	case COMP_TRIGGER_RESET:
		/* reset always succeeds */
		if (dev->state == COMP_STATE_ACTIVE ||
		    dev->state == COMP_STATE_PAUSED) {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_RESET",
						  dev->state);
			ret = 0;
		}
		dev->state = COMP_STATE_READY;
		break;
	case COMP_TRIGGER_PREPARE:
		if (dev->state == COMP_STATE_READY) {
			dev->state = COMP_STATE_PREPARE;
		} else {
			trace_comp_error_with_ids(dev, "comp_set_state() error: "
						  "wrong state = %u, "
						  "COMP_TRIGGER_PREPARE",
						  dev->state);
			ret = -EINVAL;
		}
		break;
	default:
		break;
	}

	return ret;
}

void sys_comp_init(struct sof *sof)
{
	sof->comp_drivers = platform_shared_get(&cd, sizeof(cd));

	list_init(&sof->comp_drivers->list);

	platform_shared_commit(sof->comp_drivers, sizeof(*sof->comp_drivers));
}

int comp_get_copy_limits(struct comp_dev *dev, struct comp_copy_limits *cl)
{
	/* Get source and sink buffer addresses */
	cl->source = list_first_item(&dev->bsource_list, struct comp_buffer,
				     sink_list);
	cl->sink = list_first_item(&dev->bsink_list, struct comp_buffer,
				   source_list);

	cl->frames = audio_stream_avail_frames(&cl->source->stream,
					       &cl->sink->stream);
	cl->source_frame_bytes = audio_stream_frame_bytes(&cl->source->stream);
	cl->sink_frame_bytes = audio_stream_frame_bytes(&cl->sink->stream);
	cl->source_bytes = cl->frames * cl->source_frame_bytes;
	cl->sink_bytes = cl->frames * cl->sink_frame_bytes;

	return 0;
}

static enum task_state comp_task(void *data)
{
	int ret = comp_copy(data);
	if (ret < 0)
		return SOF_TASK_STATE_COMPLETED;

	return SOF_TASK_STATE_RESCHEDULE;
}

struct comp_dev *comp_make_shared(struct comp_dev *dev)
{
	dev = rrealloc(dev, SOF_MEM_ZONE_RUNTIME, SOF_MEM_FLAG_SHARED,
		       SOF_MEM_CAPS_RAM, dev->size);
	if (!dev) {
		trace_comp_error("comp_make_shared() error: unable to realloc component");
		return NULL;
	}

	list_init(&dev->bsource_list);
	list_init(&dev->bsink_list);

	dev->is_shared = true;

	dev->task = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*dev->task));
	if (!dev->task) {
		rfree(dev);
		return NULL;
	}

	/* TODO: only timer pipelines */

	if (schedule_task_init_ll(dev->task, SOF_SCHEDULE_LL_TIMER, SOF_TASK_PRI_HIGH, comp_task,
				  dev, dev->comp.core, 0) < 0) {
		rfree(dev->task);
		rfree(dev);
		return NULL;
	}

	return dev;
}
