/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 */

/**
  * \file include/sof/audio/component.h
  * \brief Component API definition
  * \author Liam Girdwood <liam.r.girdwood@linux.intel.com>
  * \author Keyon Jie <yang.jie@linux.intel.com>
  */

#ifndef __SOF_AUDIO_COMPONENT_H__
#define __SOF_AUDIO_COMPONENT_H__

#include <sof/audio/buffer.h>
#include <sof/audio/format.h>
#include <sof/audio/pipeline.h>
#include <sof/debug/panic.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <kernel/abi.h>
#include <user/trace.h>
#include <config.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct comp_dev;
struct sof_ipc_dai_config;
struct sof_ipc_stream_posn;

/** \addtogroup component_api Component API
 *  Component API specification.
 *  @{
 */

/** \name Audio Component States
 *  @{
 */

/**
 * States may transform as below:-
 * \verbatim
 *                                  +---------------------------------------+
 *                                  |                                       |
 *                            -------------                                 |
 *                   pause    |           |    stop                         |
 *              +-------------| ACTIVITY  |---------------+                 |
 *              |             |           |               |      prepare    |
 *              |             -------------               |   +---------+   |
 *              |                ^     ^                  |   |         |   |
 *              |                |     |                  |   |         |   |
 *              v                |     |                  v   |         |   |
 *       -------------           |     |             -------------      |   |
 *       |           |   release |     |   start     |           |      |   |
 *       |   PAUSED  |-----------+     +-------------|  PREPARE  |<-----+   |
 *       |           |                               |           |          |
 *       -------------                               -------------          |
 *              |                                      ^     ^              |
 *              |               stop                   |     |              |
 *              +--------------------------------------+     |              |
 *                                                           | prepare      |
 *                            -------------                  |              |
 *                            |           |                  |              |
 *                ----------->|   READY   |------------------+              |
 *                    reset   |           |                                 |
 *                            -------------                                 |
 *                                  ^                                       |
 *                                  |                 xrun                  |
 *                                  +---------------------------------------+
 *
 * \endverbatim
 */

#define COMP_STATE_INIT		0	/**< Component being initialised */
#define COMP_STATE_READY	1       /**< Component inactive, but ready */
#define COMP_STATE_SUSPEND	2       /**< Component suspended */
#define COMP_STATE_PREPARE	3	/**< Component prepared */
#define COMP_STATE_PAUSED	4	/**< Component paused */
#define COMP_STATE_ACTIVE	5	/**< Component active */
/** @}*/

/** \name Standard Component Stream Commands
 *  TODO: use IPC versions after 1.1
 *  @{
 */
#define COMP_TRIGGER_STOP	0	/**< Stop component stream */
#define COMP_TRIGGER_START	1	/**< Start component stream */
#define COMP_TRIGGER_PAUSE	2	/**< Pause the component stream */
#define COMP_TRIGGER_RELEASE	3	/**< Release paused component stream */
#define COMP_TRIGGER_SUSPEND	4	/**< Suspend component */
#define COMP_TRIGGER_RESUME	5	/**< Resume component */
#define COMP_TRIGGER_RESET	6	/**< Reset component */
#define COMP_TRIGGER_PREPARE	7	/**< Prepare component */
#define COMP_TRIGGER_XRUN	8	/**< XRUN component */
/** @}*/

/** \name Standard Component Control Commands
 *  "Value" commands are standard ones, known to the driver while
 *  "Data" commands are opaque blobs transferred by the driver.
 *
 *  TODO: see also: ref to ipc data structures for commands.
 *  @{
 */
#define COMP_CMD_SET_VALUE	100	/**< Set value to component */
#define COMP_CMD_GET_VALUE	101     /**< Get value from component */
#define COMP_CMD_SET_DATA	102     /**< Set data to component */
#define COMP_CMD_GET_DATA	103     /**< Get data from component */
/** @}*/

/** \name MMAP IPC status
 *  @{
 */
#define COMP_CMD_IPC_MMAP_RPOS	200	/**< Host read position */
#define COMP_CMD_IPC_MMAP_PPOS	201	/**< DAI presentation position */

#define COMP_CMD_IPC_MMAP_VOL(chan)	(216 + chan)	/**< Volume */
/** @}*/

/** \name Component status
 *  @{
 */
#define COMP_STATUS_STATE_ALREADY_SET	1	/**< Comp set state status */
/** @}*/

/** \name Component attribute types
 *  @{
 */
#define COMP_ATTR_COPY_TYPE	0	/**< Comp copy type attribute */
#define COMP_ATTR_HOST_BUFFER	1	/**< Comp host buffer attribute */
/** @}*/

/** \name Trace macros
 *  @{
 */
#define trace_comp(__e, ...) \
	trace_event(TRACE_CLASS_COMP, __e, ##__VA_ARGS__)
#define trace_comp_with_ids(comp_ptr, __e, ...)			\
	trace_event_comp(TRACE_CLASS_COMP, comp_ptr,		\
			 __e, ##__VA_ARGS__)

#define tracev_comp(__e, ...) \
	tracev_event(TRACE_CLASS_COMP, __e, ##__VA_ARGS__)
#define tracev_comp_with_ids(comp_ptr, __e, ...)		\
	tracev_event_comp(TRACE_CLASS_COMP, comp_ptr,		\
			  __e, ##__VA_ARGS__)

#define trace_comp_error(__e, ...) \
	trace_error(TRACE_CLASS_COMP, __e, ##__VA_ARGS__)
#define trace_comp_error_with_ids(comp_ptr, __e, ...)		\
	trace_error_comp(TRACE_CLASS_COMP, comp_ptr,		\
			 __e, ##__VA_ARGS__)
/** @}*/

/* \brief Type of endpoint this component is connected to in a pipeline */
enum comp_endpoint_type {
	COMP_ENDPOINT_HOST,
	COMP_ENDPOINT_DAI,
	COMP_ENDPOINT_NODE
};

 /* \brief Type of component copy, which can be changed on runtime */
enum comp_copy_type {
	COMP_COPY_NORMAL = 0,
	COMP_COPY_BLOCKING,
	COMP_COPY_ONE_SHOT,
};

/**
 * Audio component operations - all mandatory.
 *
 * All component operations must return 0 for success, negative values for
 * errors and 1 to stop the pipeline walk operation.
 */
struct comp_ops {
	/** component creation */
	struct comp_dev *(*new)(struct sof_ipc_comp *comp);

	/** component destruction */
	void (*free)(struct comp_dev *dev);

	/** set component audio stream parameters */
	int (*params)(struct comp_dev *dev);

	/** set component audio stream parameters */
	int (*dai_config)(struct comp_dev *dev,
		struct sof_ipc_dai_config *dai_config);

	/** used to pass standard and bespoke commands (with optional data) */
	int (*cmd)(struct comp_dev *dev, int cmd, void *data,
		   int max_data_size);

	/** atomic - used to start/stop/pause stream operations */
	int (*trigger)(struct comp_dev *dev, int cmd);

	/** prepare component after params are set */
	int (*prepare)(struct comp_dev *dev);

	/** reset component */
	int (*reset)(struct comp_dev *dev);

	/** copy and process stream data from source to sink buffers */
	int (*copy)(struct comp_dev *dev);

	/** position */
	int (*position)(struct comp_dev *dev,
		struct sof_ipc_stream_posn *posn);

	/** cache operation on component data */
	void (*cache)(struct comp_dev *dev, int cmd);

	/** set attribute in component */
	int (*set_attribute)(struct comp_dev *dev, uint32_t type,
			     void *value);
};


/**
 * Audio component base driver "class"
 * - used by all other component types.
 */
struct comp_driver {
	uint32_t type;		/**< SOF_COMP_ for driver */
	uint32_t module_id;	/**< module id */

	struct comp_ops ops;	/**< component operations */

	struct list_item list;	/**< list of component drivers */
};

/**
 * Audio component base device "class"
 * - used by other component types.
 */
struct comp_dev {

	/* runtime */
	uint16_t state;		   /**< COMP_STATE_ */
	uint64_t position;	   /**< component rendering position */
	uint32_t frames;	   /**< number of frames we copy to sink */
	uint32_t output_rate;      /**< 0 means all output rates are fine */
	struct pipeline *pipeline; /**< pipeline we belong to */

	uint32_t min_sink_bytes;   /**< min free sink buffer size measured in
				     *  bytes required to run component's
				     *  processing
				     */
	uint32_t min_source_bytes; /**< amount of data measured in bytes
				     *  available at source buffer required
				     *  to run component's processing
				     */

	uint64_t cpc;		   /**< cycles per data chunk needed to finish
				     *  processing
				     */

	/** common runtime configuration for downstream/upstream */
	struct sof_ipc_stream_params params;

	/** driver */
	struct comp_driver *drv;

	/* lists */
	struct list_item bsource_list;	/**< list of source buffers */
	struct list_item bsink_list;	/**< list of sink buffers */

	/* private data - core does not touch this */
	void *private;		/**< private data */

	/**
	 * IPC config object header - MUST be at end as it's
	 * variable size/type
	 */
	struct sof_ipc_comp comp;
};

/** \name Helpers.
 *  @{
 */

/** \brief Struct for use with comp_get_copy_limits() function. */
struct comp_copy_limits {
	struct comp_buffer *sink;
	struct comp_buffer *source;
	int frames;
	int source_bytes;
	int sink_bytes;
	int source_frame_bytes;
	int sink_frame_bytes;
};

/** \brief Computes size of the component device including ipc config. */
#define COMP_SIZE(x) \
	(sizeof(struct comp_dev) - sizeof(struct sof_ipc_comp) + sizeof(x))

/** \brief Retrieves component device IPC configuration. */
#define COMP_GET_IPC(dev, type) \
	(struct type *)(&dev->comp)

/** \brief Retrieves component device runtime configuration. */
#define COMP_GET_PARAMS(dev) \
	(struct type *)(&dev->params)

/** \brief Retrieves component device config data. */
#define COMP_GET_CONFIG(dev) \
	(struct sof_ipc_comp_config *)((char *)&dev->comp + \
	sizeof(struct sof_ipc_comp))

/** \brief Sets the driver private data. */
#define comp_set_drvdata(c, data) \
	(c->private = data)

/** \brief Retrieves the driver private data. */
#define comp_get_drvdata(c) \
	c->private

/** \brief Retrieves the component device buffer list. */
#define comp_buffer_list(comp, dir) \
	((dir) == PPL_DIR_DOWNSTREAM ? &comp->bsink_list : \
	 &comp->bsource_list)

/** @}*/

/** \name Declare module macro
 *  \brief Usage at the end of an independent module file:
 *         DECLARE_MODULE(sys_*_init);
 *  @{
 */
#ifdef UNIT_TEST
#define DECLARE_MODULE(init)
#elif CONFIG_LIBRARY
/* In case of shared libs components are initialised in dlopen */
#define DECLARE_MODULE(init) __attribute__((constructor)) \
	static void _module_init(void) { init(); }
#else
#define DECLARE_MODULE(init) __attribute__((__used__)) \
	__attribute__((section(".module_init"))) static void(*f)(void) = init
#endif

/** @}*/

/** \name Component registration
 *  @{
 */

/**
 * Registers the component driver on the list of available components.
 * @param drv Component driver to be registered.
 * @return 0 if succeeded, error code otherwise.
 */
int comp_register(struct comp_driver *drv);

/**
 * Unregisters the component driver from the list of available components.
 * @param drv Component driver to be unregistered.
 */
void comp_unregister(struct comp_driver *drv);

/** @}*/

/** \name Component creation and destruction - mandatory
 *  @{
 */

/**
 * Creates a new component device.
 * @param comp Parameters of the new component device.
 * @return Pointer to the new component device.
 */
struct comp_dev *comp_new(struct sof_ipc_comp *comp);

/**
 * Called to delete the specified component device.
 * All data structures previously allocated on the run-time heap
 * must be freed by the implementation of <code>free()</code>.
 * @param dev Component device to be deleted.
 */
static inline void comp_free(struct comp_dev *dev)
{
	assert(dev->drv->ops.free);

	dev->drv->ops.free(dev);
}

/** @}*/

/** \name Component API.
 *  @{
 */

/**
 * Component state set.
 * @param dev Component device.
 * @param cmd Command, one of <i>COMP_TRIGGER_...</i>.
 * @return 0 if succeeded, error code otherwise.
 */
int comp_set_state(struct comp_dev *dev, int cmd);

/**
 * Component parameter init.
 * @param dev Component device.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_params(struct comp_dev *dev)
{
	if (dev->drv->ops.params)
		return dev->drv->ops.params(dev);
	return 0;
}

/**
 * Send component command.
 * @param dev Component device.
 * @param cmd Command.
 * @param data Command data.
 * @param max_data_size Max data size.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_cmd(struct comp_dev *dev, int cmd, void *data,
			   int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = data;

	if (cmd == COMP_CMD_SET_DATA &&
	    (cdata->data->magic != SOF_ABI_MAGIC ||
	     SOF_ABI_VERSION_INCOMPATIBLE(SOF_ABI_VERSION, cdata->data->abi))) {
		trace_comp_error_with_ids(dev, "comp_cmd() error: "
					  "invalid version, "
					  "data->magic = %u, data->abi = %u",
					  cdata->data->magic, cdata->data->abi);
		return -EINVAL;
	}

	if (dev->drv->ops.cmd)
		return dev->drv->ops.cmd(dev, cmd, data, max_data_size);
	return -EINVAL;
}

/**
 * Trigger component - mandatory and atomic.
 * @param dev Component device.
 * @param cmd Command.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_trigger(struct comp_dev *dev, int cmd)
{
	assert(dev->drv->ops.trigger);

	return dev->drv->ops.trigger(dev, cmd);
}

/**
 * Prepare component.
 * @param dev Component device.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_prepare(struct comp_dev *dev)
{
	if (dev->drv->ops.prepare)
		return dev->drv->ops.prepare(dev);
	return 0;
}

/**
 * Copy component buffers - mandatory.
 * @param dev Component device.
 * @return Number of frames copied.
 */
static inline int comp_copy(struct comp_dev *dev)
{
	assert(dev->drv->ops.copy);

	return dev->drv->ops.copy(dev);
}

/**
 * Component reset and free runtime resources.
 * @param dev Component device.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_reset(struct comp_dev *dev)
{
	if (dev->drv->ops.reset)
		return dev->drv->ops.reset(dev);
	return 0;
}

/**
 * DAI configuration - only mandatory for DAI components.
 * @param dev Component device.
 * @param config DAI configuration.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_dai_config(struct comp_dev *dev,
	struct sof_ipc_dai_config *config)
{
	if (dev->drv->ops.dai_config)
		return dev->drv->ops.dai_config(dev, config);
	return 0;
}

/**
 * Retrieves component rendering position.
 * @param dev Component device.
 * @param posn Position reported by the component device.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_position(struct comp_dev *dev,
	struct sof_ipc_stream_posn *posn)
{
	if (dev->drv->ops.position)
		return dev->drv->ops.position(dev, posn);
	return 0;
}

/**
 * Component L1 cache command (invalidate, writeback, ...).
 * @param dev Component device
 * @param cmd Command.
 */
static inline void comp_cache(struct comp_dev *dev, int cmd)
{
	if (dev->drv->ops.cache)
		dev->drv->ops.cache(dev, cmd);
}

/**
 * Sets component attribute.
 * @param dev Component device.
 * @param type Attribute type.
 * @param value Attribute value.
 * @return 0 if succeeded, error code otherwise.
 */
static inline int comp_set_attribute(struct comp_dev *dev, uint32_t type,
				     void *value)
{
	if (dev->drv->ops.set_attribute)
		return dev->drv->ops.set_attribute(dev, type, value);
	return 0;
}

/** @}*/

/** \name Components API for infrastructure.
 *  @{
 */

/**
 * Allocates and initializes audio component list.
 * To be called once at boot time.
 */
void sys_comp_init(void);

/** @}*/

/** \name Helpers.
 *  @{
 */

/**
 * Checks if two component devices belong to the same parent pipeline.
 * @param current Component device.
 * @param previous Another component device.
 * @return 1 if children of the same pipeline, 0 otherwise.
 */
static inline int comp_is_single_pipeline(struct comp_dev *current,
					  struct comp_dev *previous)
{
	return current->comp.pipeline_id == previous->comp.pipeline_id;
}

/**
 * Checks if component device is active.
 * @param current Component device.
 * @return 1 if active, 0 otherwise.
 */
static inline int comp_is_active(struct comp_dev *current)
{
	return current->state == COMP_STATE_ACTIVE;
}

/**
 * Calculates period size in bytes based on component device's parameters.
 * @param dev Component device.
 * @return Period size in bytes.
 */
static inline uint32_t comp_frame_bytes(struct comp_dev *dev)
{
	return frame_bytes(dev->params.frame_fmt, dev->params.channels);
}

/**
 * Calculates sample size in bytes based on component device's parameters.
 * @param dev Component device.
 * @return Size of sample in bytes.
 */
static inline uint32_t comp_sample_bytes(struct comp_dev *dev)
{
	return sample_bytes(dev->params.frame_fmt);
}

/**
 * Calculates period size in bytes based on component device's parameters.
 * @param dev Component device.
 * @param frames Number of processing frames.
 * @return Period size in bytes.
 */
static inline uint32_t comp_period_bytes(struct comp_dev *dev, uint32_t frames)
{
	return frames * comp_frame_bytes(dev);
}

static inline uint32_t comp_avail_frames(struct comp_buffer *source,
					 struct comp_buffer *sink)
{
	uint32_t src_frames = source->avail / comp_frame_bytes(source->source);
	uint32_t sink_frames = sink->free / comp_frame_bytes(sink->sink);

	return MIN(src_frames, sink_frames);
}

/**
 * Returns component state based on requested command.
 * @param cmd Request command.
 * @return Component state.
 */
static inline int comp_get_requested_state(int cmd)
{
	int state = COMP_STATE_INIT;

	switch (cmd) {
	case COMP_TRIGGER_START:
	case COMP_TRIGGER_RELEASE:
		state = COMP_STATE_ACTIVE;
		break;
	case COMP_TRIGGER_PREPARE:
	case COMP_TRIGGER_STOP:
		state = COMP_STATE_PREPARE;
		break;
	case COMP_TRIGGER_PAUSE:
		state = COMP_STATE_PAUSED;
		break;
	case COMP_TRIGGER_XRUN:
	case COMP_TRIGGER_RESET:
		state = COMP_STATE_READY;
		break;
	default:
		break;
	}

	return state;
}

/* \brief Returns comp_endpoint_type of given component */
static inline int comp_get_endpoint_type(struct comp_dev *dev)
{
	switch (dev->comp.type) {
	case SOF_COMP_HOST:
		return COMP_ENDPOINT_HOST;
	case SOF_COMP_DAI:
		return COMP_ENDPOINT_DAI;
	default:
		return COMP_ENDPOINT_NODE;
	}
}

/** @}*/

/** \name XRUN handling.
 *  @{
 */

/**
 * Called by the component device when underrun is detected.
 * @param dev Component device.
 * @param source Source buffer.
 * @param copy_bytes Requested size of data to be available.
 */
static inline void comp_underrun(struct comp_dev *dev,
				 struct comp_buffer *source,
				 uint32_t copy_bytes)
{
	trace_comp_error_with_ids(dev, "comp_underrun() error: "
				  "dev->comp.id = %u, "
				  "source->avail = %u, "
				  "copy_bytes = %u",
				  dev->comp.id,
				  source->avail,
				  copy_bytes);

	pipeline_xrun(dev->pipeline, dev, (int32_t)source->avail - copy_bytes);
}

/**
 * Called by component device when overrun is detected.
 * @param dev Component device.
 * @param sink Sink buffer.
 * @param copy_bytes Requested size of free space to be available.
 */
static inline void comp_overrun(struct comp_dev *dev, struct comp_buffer *sink,
				uint32_t copy_bytes)
{
	trace_comp_error("comp_overrun() error: dev->comp.id = %u, sink->free "
			 "= %u, copy_bytes = %u", dev->comp.id, sink->free,
			 copy_bytes);

	pipeline_xrun(dev->pipeline, dev, (int32_t)copy_bytes - sink->free);
}

/**
 * Called to check whether component schedules its pipeline.
 * @param dev Component device.
 * @return True if this is scheduling component, false otherwise.
 */
static inline bool comp_is_scheduling_source(struct comp_dev *dev)
{
	return dev == dev->pipeline->sched_comp;
}

/**
 * Called to check whether component works in low latency processing domain.
 * @param dev Component device.
 * @return True if this is low latency component, false otherwise.
 */
static inline bool comp_is_low_latency(struct comp_dev *dev)
{
	struct sof_ipc_comp_config *config = COMP_GET_CONFIG(dev);

	return config->proc_domain == SOF_COMP_PROC_DOMAIN_LL;
}

/**
 * Called to check whether component is ready to process its data.
 * @param dev Component device.
 * @return True if component is ready, false otherwise.
 */
static inline bool comp_is_copy_ready(struct comp_dev *dev)
{
	struct comp_buffer *source;
	struct comp_buffer *sink;

	/* component requires minimal number of input bytes to run */
	if (dev->min_source_bytes) {
		source = list_first_item(&dev->bsource_list, struct comp_buffer,
					 sink_list);
		if (source->avail < dev->min_source_bytes) {
			tracev_comp_with_ids(dev,
					     "comp_is_copy_ready(): not enough input bytes: %u",
					     source->avail);
			return false;
		}
	}

	/* component requires minimal output free space */
	if (dev->min_sink_bytes) {
		sink = list_first_item(&dev->bsink_list, struct comp_buffer,
				       source_list);
		if (sink->free < dev->min_sink_bytes) {
			tracev_comp_with_ids(dev,
					     "comp_is_copy_ready(): not enough output bytes: %u",
					     sink->free);
			return false;
		}
	}

	return true;
}

/**
 * Called by component in copy.
 * @param dev Component device.
 * @param cl Struct of parameters for use in copy function.
 */
int comp_get_copy_limits(struct comp_dev *dev, struct comp_copy_limits *cl);

/** @}*/

/** @}*/

#endif /* __SOF_AUDIO_COMPONENT_H__ */
