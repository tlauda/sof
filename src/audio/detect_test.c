// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Slawomir Blauciak <slawomir.blauciak@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/format.h>
#include <sof/audio/kpb.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/drivers/ipc.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/lib/wait.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/string.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <kernel/abi.h>
#include <user/detect_test.h>
#include <user/trace.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* tracing */
#define trace_keyword(__e, ...) \
	trace_event(TRACE_CLASS_KEYWORD, __e, ##__VA_ARGS__)
#define trace_keyword_with_ids(comp_ptr, __e, ...)		\
	trace_event_comp(TRACE_CLASS_KEYWORD, comp_ptr,		\
			 __e, ##__VA_ARGS__)

#define tracev_keyword(__e, ...) \
	tracev_event(TRACE_CLASS_KEYWORD, __e, ##__VA_ARGS__)
#define tracev_keyword_with_ids(comp_ptr, __e, ...)		\
	tracev_event_comp(TRACE_CLASS_KEYWORD, comp_ptr,	\
			  __e, ##__VA_ARGS__)

#define trace_keyword_error(__e, ...) \
	trace_error(TRACE_CLASS_KEYWORD, __e, ##__VA_ARGS__)
#define trace_keyword_error_with_ids(comp_ptr, __e, ...)	\
	trace_error_comp(TRACE_CLASS_KEYWORD, comp_ptr,		\
			 __e, ##__VA_ARGS__)

#define ACTIVATION_DEFAULT_SHIFT 3
#define ACTIVATION_DEFAULT_DIVIDER_S16 0.5
#define ACTIVATION_DEFAULT_DIVIDER_S24 0.05

#define ACTIVATION_DEFAULT_THRESHOLD_S16 \
	((int16_t)((INT16_MAX) * (ACTIVATION_DEFAULT_DIVIDER_S16)))
#define ACTIVATION_DEFAULT_THRESHOLD_S24 \
	((int32_t)((INT24_MAXVALUE) * (ACTIVATION_DEFAULT_DIVIDER_S24)))
#define ACTIVATION_DEFAULT_THRESHOLD_S32 \
	ACTIVATION_DEFAULT_THRESHOLD_S24

#define INITIAL_MODEL_DATA_SIZE 64

/* default number of samples before detection is activated  */
#define KEYPHRASE_DEFAULT_PREAMBLE_LENGTH 0

struct model_data {
	uint32_t data_size;
	void *data;
	uint32_t crc;
	uint32_t data_pos; /**< current copy position for model data */
};

struct comp_data {
	struct sof_detect_test_config config;
	struct model_data model;
	int32_t activation;
	uint32_t detected;
	uint32_t detect_preamble; /**< current keyphrase preamble length */
	uint32_t keyphrase_samples; /**< keyphrase length in samples */
	uint32_t history_depth; /** defines draining size in bytes. */

	uint16_t sample_valid_bytes;
	struct kpb_event_data event_data;
	struct kpb_client client_data;

	void (*detect_func)(struct comp_dev *dev,
			    struct comp_buffer *source, uint32_t frames);
};

static inline bool detector_is_sample_width_supported(enum sof_ipc_frame sf)
{
	bool ret;

	switch (sf) {
#if CONFIG_FORMAT_S16LE
	case SOF_IPC_FRAME_S16_LE:
		/* FALLTHRU */
#endif /* CONFIG_FORMAT_S16LE */
#if CONFIG_FORMAT_S24LE
	case SOF_IPC_FRAME_S24_4LE:
		/* FALLTHRU */
#endif /* CONFIG_FORMAT_S24LE */
#if CONFIG_FORMAT_S32LE
	case SOF_IPC_FRAME_S32_LE:
		/* FALLTHRU */
#endif /* CONFIG_FORMAT_S32LE */
		ret = true;
		break;
	default:
		ret = false;
		break;
	}

	return ret;
}

static void notify_host(struct comp_dev *dev)
{
	struct sof_ipc_comp_event event;

	trace_keyword_with_ids(dev, "notify_host()");

	event.event_type = SOF_CTRL_EVENT_KD;
	event.num_elems = 0;

	ipc_send_comp_notification(dev, &event);

	/* Send queued IPC message right away to wake host up ASAP
	 * NOTE! This will only send one IPC from the list!
	 */
	ipc_platform_send_msg();
}

static void notify_kpb(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_keyword_with_ids(dev, "notify_kpb(), preamble: %u",
			       cd->detect_preamble);

	cd->client_data.r_ptr = NULL;
	cd->client_data.sink = NULL;
	cd->client_data.id = 0; /**< TODO: acquire proper id from kpb */
	/* time in milliseconds */
	cd->client_data.history_depth = (cd->history_depth != 0) ?
					 cd->history_depth :
					 cd->config.history_depth;
	cd->event_data.event_id = KPB_EVENT_BEGIN_DRAINING;
	cd->event_data.client_data = &cd->client_data;

	notifier_event(dev, NOTIFIER_ID_KPB_CLIENT_EVT,
		       NOTIFIER_TARGET_CORE_ALL_MASK, &cd->event_data,
		       sizeof(cd->event_data));
}

static void detect_test_notify(struct comp_dev *dev)
{
	notify_host(dev);
	notify_kpb(dev);
}

static void default_detect_test(struct comp_dev *dev,
				struct comp_buffer *source, uint32_t frames)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	void *src;
	int32_t diff;
	uint32_t count = frames; /**< Assuming single channel */
	uint32_t sample;
	uint16_t valid_bits = cd->sample_valid_bytes * 8;
	const int32_t activation_threshold = cd->config.activation_threshold;
	uint32_t cycles_per_frame; /**< Clock cycles required per frame */

	/* synthetic load */
	if (cd->config.load_mips) {
		/* assuming count is a processing frame size in samples */
		cycles_per_frame = (cd->config.load_mips * 1000000 * count)
				   / source->rate;
		idelay(cycles_per_frame);
	}

	/* perform detection within current period */
	for (sample = 0; sample < count && !cd->detected; ++sample) {
		src = (valid_bits == 16U) ?
		      buffer_read_frag_s16(source, sample) :
		      buffer_read_frag_s32(source, sample);
		if (valid_bits > 16U) {
			diff = abs(*(int32_t *)src) - abs(cd->activation);
		} else {
			diff = abs(*(int16_t *)src) -
			       abs((int16_t)cd->activation);
		}

		diff >>= cd->config.activation_shift;
		cd->activation += diff;

		if (cd->detect_preamble >= cd->keyphrase_samples) {
			if (cd->activation >= activation_threshold) {
				/* The algorithm shall use cd->history_depth
				 * to specify its draining size request.
				 * Zero value means default config value
				 * will be used.
				 */
				cd->history_depth = 0;
				detect_test_notify(dev);
				cd->detected = 1;
			}
		} else {
			++cd->detect_preamble;
		}
	}
}

static void free_mem_load(struct comp_data *cd)
{
	if (!cd) {
		trace_keyword_error("free_mem_load() error: invalid cd");
		return;
	}

	if (cd->model.data) {
		rfree(cd->model.data);
		cd->model.data = NULL;
		cd->model.data_size = 0;
		cd->model.crc = 0;
		cd->model.data_pos = 0;
	}
}

static int alloc_mem_load(struct comp_data *cd, uint32_t size)
{
	if (!size)
		return 0;

	if (!cd) {
		trace_keyword_error("alloc_mem_load() error: invalid cd");
		return -EINVAL;
	}

	free_mem_load(cd);

	cd->model.data = rballoc(0, SOF_MEM_CAPS_RAM, size);

	if (!cd->model.data) {
		trace_keyword_error("alloc_mem_load() alloc failed");
		return -ENOMEM;
	}

	bzero(cd->model.data, size);
	cd->model.data_size = size;
	cd->model.data_pos = 0;

	return 0;
}

static int test_keyword_get_threshold(struct comp_dev *dev, int sample_width)
{
	switch (sample_width) {
#if CONFIG_FORMAT_S16LE
	case 16:
		return ACTIVATION_DEFAULT_THRESHOLD_S16;
#endif /* CONFIG_FORMAT_S16LE */
#if CONFIG_FORMAT_S24LE
	case 24:
		return ACTIVATION_DEFAULT_THRESHOLD_S24;
#endif /* CONFIG_FORMAT_S24LE */
#if CONFIG_FORMAT_S32LE
	case 32:
		return ACTIVATION_DEFAULT_THRESHOLD_S32;
#endif /* CONFIG_FORMAT_S32LE */
	default:
		trace_keyword_error_with_ids(dev, "test_keyword_get_threshold(), unsupported sample width: %d",
					     sample_width);
		return -EINVAL;
	}
}

static int test_keyword_apply_config(struct comp_dev *dev,
				     struct sof_detect_test_config *cfg)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	uint16_t sample_width;
	int ret;

	ret = memcpy_s(&cd->config, sizeof(cd->config), cfg,
		       sizeof(struct sof_detect_test_config));
	assert(!ret);

	sample_width = cd->config.sample_width;

	if (!cd->config.activation_shift)
		cd->config.activation_shift = ACTIVATION_DEFAULT_SHIFT;

	if (!cd->config.activation_threshold) {
		cd->config.activation_threshold =
			test_keyword_get_threshold(dev, sample_width);
	}

	return 0;
}

static struct comp_dev *test_keyword_new(struct sof_ipc_comp *comp)
{
	struct comp_dev *dev;
	struct sof_ipc_comp_process *keyword;
	struct sof_ipc_comp_process *ipc_keyword =
		(struct sof_ipc_comp_process *)comp;
	struct comp_data *cd;
	struct sof_detect_test_config *cfg;
	int ret = 0;
	size_t bs;

	trace_keyword("test_keyword_new()");

	if (IPC_IS_SIZE_INVALID(ipc_keyword->config)) {
		IPC_SIZE_ERROR_TRACE(TRACE_CLASS_KEYWORD, ipc_keyword->config);
		return NULL;
	}

	dev = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
		      COMP_SIZE(struct sof_ipc_comp_process));
	if (!dev)
		return NULL;

	keyword = (struct sof_ipc_comp_process *)&dev->comp;
	ret = memcpy_s(keyword, sizeof(*keyword), ipc_keyword,
		       sizeof(struct sof_ipc_comp_process));
	assert(!ret);

	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));

	if (!cd)
		goto fail;

	/* using default processing function */
	cd->detect_func = default_detect_test;

	comp_set_drvdata(dev, cd);

	cfg = (struct sof_detect_test_config *)ipc_keyword->data;
	bs = ipc_keyword->size;

	if (bs > 0) {
		if (bs < sizeof(struct sof_detect_test_config)) {
			trace_keyword_error_with_ids(dev, "test_keyword_new() "
						     "error: invalid data size");
			goto fail;
		}

		if (test_keyword_apply_config(dev, cfg)) {
			trace_keyword_error_with_ids(dev, "test_keyword_new() "
						     "error: failed to apply config");
			goto fail;
		}
	}

	ret = alloc_mem_load(cd, INITIAL_MODEL_DATA_SIZE);
	if (ret < 0) {
		trace_keyword_error_with_ids(dev, "test_keyword_new() "
					     "error: model data initial failed");
		goto fail;
	}

	dev->state = COMP_STATE_READY;
	return dev;

fail:
	if (cd)
		rfree(cd);
	rfree(dev);
	return NULL;
}

static void test_keyword_free(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_keyword_with_ids(dev, "test_keyword_free()");

	free_mem_load(cd);
	rfree(cd);
	rfree(dev);
}

/* set component audio stream parameters */
static int test_keyword_params(struct comp_dev *dev,
			       struct sof_ipc_stream_params *params)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sourceb;

	cd->sample_valid_bytes = params->sample_valid_bytes;

	/* keyword components will only ever have 1 source */
	sourceb = list_first_item(&dev->bsource_list, struct comp_buffer,
				  sink_list);

	/* TODO: remove in the future */
	sourceb->channels = 1;

	if (sourceb->channels != 1) {
		trace_keyword_error_with_ids(dev, "test_keyword_params() "
					     "error: only single-channel "
					     "supported");
		return -EINVAL;
	}

	if (!detector_is_sample_width_supported(sourceb->frame_fmt)) {
		trace_keyword_error_with_ids(dev, "test_keyword_params() "
					     "error: only 16-bit format "
					     "supported");
		return -EINVAL;
	}

	/* calculate the length of the preamble */
	if (cd->config.preamble_time) {
		cd->keyphrase_samples = cd->config.preamble_time *
					(sourceb->rate / 1000);
	} else {
		cd->keyphrase_samples = KEYPHRASE_DEFAULT_PREAMBLE_LENGTH;
	}

	return 0;
}

static int test_keyword_set_config(struct comp_dev *dev,
				   struct sof_ipc_ctrl_data *cdata)
{
	struct sof_detect_test_config *cfg;
	size_t bs;

	/* Copy new config, find size from header */
	cfg = (struct sof_detect_test_config *)cdata->data->data;
	bs = cfg->size;

	trace_keyword_with_ids(dev, "test_keyword_set_config(), blob size = %u",
			       bs);

	if (bs != sizeof(struct sof_detect_test_config)) {
		trace_keyword_error_with_ids(dev, "test_keyword_set_config() "
					     "error: invalid blob size");
		return -EINVAL;
	}

	return test_keyword_apply_config(dev, cfg);
}

static int test_keyword_set_model(struct comp_dev *dev,
				  struct sof_ipc_ctrl_data *cdata)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int ret = 0;
	bool done = false;

	tracev_keyword_with_ids(dev, "keyword_ctrl_set_model() "
				"msg_index = %d, num_elems = %d, "
				"remaining = %d ",
				cdata->msg_index, cdata->num_elems,
				cdata->elems_remaining);

	if (!cdata->msg_index) {
		ret = alloc_mem_load(cd, cdata->data->size);
		if (ret < 0)
			return ret;
	}

	if (!cd->model.data) {
		trace_keyword_error_with_ids(dev, "keyword_ctrl_set_model() "
					    "error: buffer not allocated");
		return -EINVAL;
	}

	if (!cdata->elems_remaining) {
		if (cdata->num_elems + cd->model.data_pos <
		    cd->model.data_size) {
			trace_keyword_error_with_ids(dev,
						     "keyword_ctrl_set_model() error: "
						     "not enough data to fill the buffer");

			/* TODO: anything to do in such a situation? */

			return -EINVAL;
		}

		done = true;
		trace_keyword_with_ids(dev, "test_keyword_set_model() "
				       "final packet received");
	}

	if (cdata->num_elems >
	    cd->model.data_size - cd->model.data_pos) {
		trace_keyword_error_with_ids(dev, "keyword_ctrl_set_model() "
					     "error: too much data");
		return -EINVAL;
	}

	ret = memcpy_s((char *)cd->model.data + cd->model.data_pos,
		       cd->model.data_size - cd->model.data_pos,
		       cdata->data->data, cdata->num_elems);
	assert(!ret);

	cd->model.data_pos += cdata->num_elems;

	if (done) {
		/* Set model data done, update crc value */
		cd->model.crc = crc32(0, cd->model.data,
				      cd->model.data_size);
		trace_keyword_with_ids(dev, "keyword_ctrl_set_model() "
				       "done, memory_size = 0x%x, crc = 0x%08x",
				       cd->model.data_size, cd->model.crc);
	}
	return 0;
}

static int test_keyword_ctrl_set_bin_data(struct comp_dev *dev,
					  struct sof_ipc_ctrl_data *cdata)
{
	int ret = 0;

	if (dev->state != COMP_STATE_READY) {
		/* It is a valid request but currently this is not
		 * supported during playback/capture. The driver will
		 * re-send data in next resume when idle and the new
		 * configuration will be used when playback/capture
		 * starts.
		 */
		trace_keyword_error_with_ids(dev, "keyword_ctrl_set_bin_data() "
					     "error: driver is busy");
		return -EBUSY;
	}

	switch (cdata->data->type) {
	case SOF_DETECT_TEST_CONFIG:
		ret = test_keyword_set_config(dev, cdata);
		break;
	case SOF_DETECT_TEST_MODEL:
		ret = test_keyword_set_model(dev, cdata);
		break;
	default:
		trace_keyword_error_with_ids(dev, "keyword_ctrl_set_bin_data() "
					     "error: unknown binary data type");
		break;
	}

	return ret;
}

static int test_keyword_ctrl_set_data(struct comp_dev *dev,
				      struct sof_ipc_ctrl_data *cdata)
{
	int ret = 0;

	/* Check version from ABI header */
	if (SOF_ABI_VERSION_INCOMPATIBLE(SOF_ABI_VERSION, cdata->data->abi)) {
		trace_keyword_error_with_ids(dev, "test_keyword_cmd_set_data() "
					     "error: invalid version");
		return -EINVAL;
	}

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_ENUM:
		trace_keyword_with_ids(dev, "test_keyword_cmd_set_data(), "
				       "SOF_CTRL_CMD_ENUM");
		break;
	case SOF_CTRL_CMD_BINARY:
		trace_keyword_with_ids(dev, "test_keyword_cmd_set_data(), "
				       "SOF_CTRL_CMD_BINARY");
		ret = test_keyword_ctrl_set_bin_data(dev, cdata);
		break;
	default:
		trace_keyword_error_with_ids(dev, "test_keyword_cmd_set_data() "
					     "error: invalid cdata->cmd");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int test_keyword_get_config(struct comp_dev *dev,
				   struct sof_ipc_ctrl_data *cdata, int size)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	size_t bs;
	int ret = 0;

	trace_keyword_with_ids(dev, "test_keyword_get_config()");

	/* Copy back to user space */
	bs = cd->config.size;
	trace_keyword_with_ids(dev, "value of block size: %u", bs);

	if (bs == 0 || bs > size)
		return -EINVAL;

	ret = memcpy_s(cdata->data->data, size, &cd->config, bs);
	assert(!ret);

	cdata->data->abi = SOF_ABI_VERSION;
	cdata->data->size = bs;

	return ret;
}

static int test_keyword_get_model(struct comp_dev *dev,
				  struct sof_ipc_ctrl_data *cdata, int size)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	size_t bs;
	int ret = 0;

	tracev_keyword_with_ids(dev, "test_keyword_get_model() "
				"msg_index = %d, num_elems = %d, "
				"remaining = %d ",
				cdata->msg_index, cdata->num_elems,
				cdata->elems_remaining);

	/* Copy back to user space */
	if (cd->model.data) {
		if (!cdata->msg_index) {
			/* reset copy offset */
			cd->model.data_pos = 0;
			trace_keyword_with_ids(dev, "test_keyword_get_model() "
					       "model data_size = 0x%x, "
					       "crc = 0x%08x",
					       cd->model.data_size,
					       cd->model.crc);
		}

		bs = cdata->num_elems;
		if (bs > size) {
			trace_keyword_error_with_ids(dev,
						     "test_keyword_get_model() "
						     "error: "
						     "invalid size %d", bs);
			return -EINVAL;
		}

		ret = memcpy_s(cdata->data->data, size,
			       (char *)cd->model.data + cd->model.data_pos,
			       bs);
		assert(!ret);

		cdata->data->abi = SOF_ABI_VERSION;
		cdata->data->size = cd->model.data_size;
		cd->model.data_pos += bs;

	} else {
		trace_keyword_error_with_ids(dev, "test_keyword_get_model() "
					     "error: invalid cd->config");
		ret = -EINVAL;
	}

	return ret;
}

static int test_keyword_ctrl_get_bin_data(struct comp_dev *dev,
					  struct sof_ipc_ctrl_data *cdata,
					  int size)
{
	int ret = 0;

	switch (cdata->data->type) {
	case SOF_DETECT_TEST_CONFIG:
		ret = test_keyword_get_config(dev, cdata, size);
		break;
	case SOF_DETECT_TEST_MODEL:
		ret = test_keyword_get_model(dev, cdata, size);
		break;
	default:
		trace_keyword_error_with_ids(dev,
					     "test_keyword_ctrl_get_bin_data() "
					     "error: "
					     "unknown binary data type");
		break;
	}

	return ret;
}

static int test_keyword_ctrl_get_data(struct comp_dev *dev,
				      struct sof_ipc_ctrl_data *cdata, int size)
{
	int ret = 0;

	trace_keyword_with_ids(dev, "test_keyword_ctrl_get_data() size: %d",
			       size);

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_BINARY:
		ret = test_keyword_ctrl_get_bin_data(dev, cdata, size);
		break;
	default:
		trace_keyword_error_with_ids(dev, "test_keyword_ctrl_get_data() "
					     "error: invalid cdata->cmd");
		return -EINVAL;
	}

	return ret;
}

/* used to pass standard and bespoke commands (with data) to component */
static int test_keyword_cmd(struct comp_dev *dev, int cmd, void *data,
			    int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = data;

	trace_keyword_with_ids(dev, "test_keyword_cmd()");

	switch (cmd) {
	case COMP_CMD_SET_DATA:
		return test_keyword_ctrl_set_data(dev, cdata);
	case COMP_CMD_GET_DATA:
		return test_keyword_ctrl_get_data(dev, cdata, max_data_size);
	default:
		return -EINVAL;
	}
}

static int test_keyword_trigger(struct comp_dev *dev, int cmd)
{
	int ret;
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_keyword_with_ids(dev, "test_keyword_trigger()");

	ret = comp_set_state(dev, cmd);
	if (ret)
		return ret;

	if (cmd == COMP_TRIGGER_START ||
	    cmd == COMP_TRIGGER_RELEASE) {
		cd->detect_preamble = 0;
		cd->detected = 0;
		cd->activation = 0;
	}

	return ret;
}

/*  process stream data from source buffer */
static int test_keyword_copy(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *source;

	tracev_keyword_with_ids(dev, "test_keyword_copy()");

	/* keyword components will only ever have 1 source */
	source = list_first_item(&dev->bsource_list,
				 struct comp_buffer, sink_list);

	/* copy and perform detection */
	cd->detect_func(dev, source,
			source->avail / buffer_frame_bytes(source));

	/* calc new available */
	comp_update_buffer_consume(source, source->avail);

	return 0;
}

static int test_keyword_reset(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_keyword_with_ids(dev, "test_keyword_reset()");

	cd->activation = 0;
	cd->detect_preamble = 0;
	cd->detected = 0;

	return comp_set_state(dev, COMP_TRIGGER_RESET);
}

static int test_keyword_prepare(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	uint16_t valid_bits = cd->sample_valid_bytes * 8;
	uint16_t sample_width = cd->config.sample_width;

	trace_keyword_with_ids(dev, "test_keyword_prepare()");

	if (valid_bits != sample_width) {
		/* Default threshold value has to be changed
		 * according to host new format.
		 */
		cd->config.activation_threshold =
			test_keyword_get_threshold(dev, valid_bits);
	}

	return comp_set_state(dev, COMP_TRIGGER_PREPARE);
}

static const struct comp_driver comp_keyword = {
	.type	= SOF_COMP_KEYWORD_DETECT,
	.ops	= {
		.new		= test_keyword_new,
		.free		= test_keyword_free,
		.params		= test_keyword_params,
		.cmd		= test_keyword_cmd,
		.trigger	= test_keyword_trigger,
		.copy		= test_keyword_copy,
		.prepare	= test_keyword_prepare,
		.reset		= test_keyword_reset,
	},
};

static SHARED_DATA struct comp_driver_info comp_keyword_info = {
	.drv = &comp_keyword,
};

static void sys_comp_keyword_init(void)
{
	comp_register(platform_shared_get(&comp_keyword_info,
					  sizeof(comp_keyword_info)));
}

DECLARE_MODULE(sys_comp_keyword_init);
