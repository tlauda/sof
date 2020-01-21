// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2017 Intel Corporation. All rights reserved.
//
// Author: Seppo Ingalsuo <seppo.ingalsuo@linux.intel.com>
//         Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/common.h>
#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/src/src.h>
#include <sof/audio/src/src_config.h>
#include <sof/debug/panic.h>
#include <sof/drivers/ipc.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/platform.h>
#include <sof/string.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <user/trace.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if SRC_SHORT
#include <sof/audio/coefficients/src/src_tiny_int16_define.h>
#include <sof/audio/coefficients/src/src_tiny_int16_table.h>
#else
#include <sof/audio/coefficients/src/src_std_int32_define.h>
#include <sof/audio/coefficients/src/src_std_int32_table.h>
#endif

/* tracing */
#define trace_src(__e, ...) \
	trace_event(TRACE_CLASS_SRC, __e, ##__VA_ARGS__)
#define trace_src_with_ids(comp_ptr, __e, ...)			\
	trace_event_comp(TRACE_CLASS_SRC, comp_ptr,		\
			 __e, ##__VA_ARGS__)

#define tracev_src(__e, ...) \
	tracev_event(TRACE_CLASS_SRC, __e, ##__VA_ARGS__)
#define tracev_src_with_ids(comp_ptr, __e, ...)			\
	tracev_event_comp(TRACE_CLASS_SRC, comp_ptr,		\
			  __e, ##__VA_ARGS__)

#define trace_src_error(__e, ...) \
	trace_error(TRACE_CLASS_SRC, __e, ##__VA_ARGS__)
#define trace_src_error_with_ids(comp_ptr, __e, ...)		\
	trace_error_comp(TRACE_CLASS_SRC, comp_ptr,		\
			 __e, ##__VA_ARGS__)

/* The FIR maximum lengths are per channel so need to multiply them */
#define MAX_FIR_DELAY_SIZE_XNCH (PLATFORM_MAX_CHANNELS * MAX_FIR_DELAY_SIZE)
#define MAX_OUT_DELAY_SIZE_XNCH (PLATFORM_MAX_CHANNELS * MAX_OUT_DELAY_SIZE)

/* src component private data */
struct comp_data {
	struct polyphase_src src;
	struct src_param param;
	int32_t *delay_lines;
	uint32_t sink_rate;
	uint32_t source_rate;
	uint32_t sink_format;
	uint32_t source_format;
	int32_t *sbuf_w_ptr;
	int32_t *sbuf_r_ptr;
	int sbuf_avail;
	int data_shift;
	int source_frames;
	int sink_frames;
	int sample_container_bytes;
	void (*src_func)(struct comp_dev *dev,
			 struct comp_buffer *source,
			 struct comp_buffer *sink,
			 int *consumed,
			 int *produced);
	void (*polyphase_func)(struct src_stage_prm *s);
};

/* Calculates the needed FIR delay line length */
static int src_fir_delay_length(struct src_stage *s)
{
	return s->subfilter_length + (s->num_of_subfilters - 1) * s->idm
		+ s->blk_in;
}

/* Calculates the FIR output delay line length */
static int src_out_delay_length(struct src_stage *s)
{
	return 1 + (s->num_of_subfilters - 1) * s->odm;
}

/* Returns index of a matching sample rate */
static int src_find_fs(int fs_list[], int list_length, int fs)
{
	int i;

	for (i = 0; i < list_length; i++) {
		if (fs_list[i] == fs)
			return i;
	}
	return -EINVAL;
}

/* Calculates buffers to allocate for a SRC mode */
int src_buffer_lengths(struct src_param *a, int fs_in, int fs_out, int nch,
		       int source_frames)
{
	struct src_stage *stage1;
	struct src_stage *stage2;
	int r1;

	if (nch > PLATFORM_MAX_CHANNELS) {
		trace_src_error("src_buffer_lengths() error: "
				"nch = %u > PLATFORM_MAX_CHANNELS", nch);
		return -EINVAL;
	}

	a->nch = nch;
	a->idx_in = src_find_fs(src_in_fs, NUM_IN_FS, fs_in);
	a->idx_out = src_find_fs(src_out_fs, NUM_OUT_FS, fs_out);

	/* Check that both in and out rates are supported */
	if (a->idx_in < 0 || a->idx_out < 0) {
		trace_src_error("src_buffer_lengths() error: "
				"rates not supported, "
				"fs_in: %u, fs_out: %u", fs_in, fs_out);
		return -EINVAL;
	}

	stage1 = src_table1[a->idx_out][a->idx_in];
	stage2 = src_table2[a->idx_out][a->idx_in];

	/* Check from stage1 parameter for a deleted in/out rate combination.*/
	if (stage1->filter_length < 1) {
		trace_src_error("src_buffer_lengths() error: "
				"Non-supported combination "
				"fs_in = %d, fs_out = %d", fs_in, fs_out);
		return -EINVAL;
	}

	a->fir_s1 = nch * src_fir_delay_length(stage1);
	a->out_s1 = nch * src_out_delay_length(stage1);

	/* Computing of number of blocks to process is done in
	 * copy() per each frame.
	 */
	a->stage1_times = 0;
	a->stage2_times = 0;
	a->blk_in = 0;
	a->blk_out = 0;

	if (stage2->filter_length == 1) {
		a->fir_s2 = 0;
		a->out_s2 = 0;
		a->sbuf_length = 0;
	} else {
		a->fir_s2 = nch * src_fir_delay_length(stage2);
		a->out_s2 = nch * src_out_delay_length(stage2);

		/* Stage 1 is repeated max. amount that just exceeds one
		 * period.
		 */
		r1 = source_frames / stage1->blk_in + 1;

		/* Set sbuf length to allow storing two stage 1 output
		 * periods. This is an empirically found value for no
		 * xruns to happen with SRC in/out buffers. Due to
		 * variable number of blocks to process per each stage
		 * there is no equation known for minimum size.
		 */
		a->sbuf_length = 2 * nch * stage1->blk_out * r1;
	}

	a->src_multich = a->fir_s1 + a->fir_s2 + a->out_s1 + a->out_s2;
	a->total = a->sbuf_length + a->src_multich;

	return 0;
}

static void src_state_reset(struct src_state *state)
{
	state->fir_delay_size = 0;
	state->out_delay_size = 0;
}

static int init_stages(struct src_stage *stage1, struct src_stage *stage2,
		       struct polyphase_src *src, struct src_param *p,
		       int n, int32_t *delay_lines_start)
{
	/* Clear FIR state */
	src_state_reset(&src->state1);
	src_state_reset(&src->state2);

	src->number_of_stages = n;
	src->stage1 = stage1;
	src->stage2 = stage2;
	if (n == 1 && stage1->blk_out == 0)
		return -EINVAL;

	/* Optimized SRC requires subfilter length multiple of 4 */
	if (stage1->filter_length > 1 && (stage1->subfilter_length & 0x3) > 0)
		return -EINVAL;

	if (stage2->filter_length > 1 && (stage2->subfilter_length & 0x3) > 0)
		return -EINVAL;

	/* Delay line sizes */
	src->state1.fir_delay_size = p->fir_s1;
	src->state1.out_delay_size = p->out_s1;
	src->state1.fir_delay = delay_lines_start;
	src->state1.out_delay =
		src->state1.fir_delay + src->state1.fir_delay_size;
	/* Initialize to last ensures that circular wrap cannot happen
	 * mid-frame. The size is multiple of channels count.
	 */
	src->state1.fir_wp = &src->state1.fir_delay[p->fir_s1 - 1];
	src->state1.out_rp = src->state1.out_delay;
	if (n > 1) {
		src->state2.fir_delay_size = p->fir_s2;
		src->state2.out_delay_size = p->out_s2;
		src->state2.fir_delay =
			src->state1.out_delay + src->state1.out_delay_size;
		src->state2.out_delay =
			src->state2.fir_delay + src->state2.fir_delay_size;
		/* Initialize to last ensures that circular wrap cannot happen
		 * mid-frame. The size is multiple of channels count.
		 */
		src->state2.fir_wp = &src->state2.fir_delay[p->fir_s2 - 1];
		src->state2.out_rp = src->state2.out_delay;
	} else {
		src->state2.fir_delay_size = 0;
		src->state2.out_delay_size = 0;
		src->state2.fir_delay = NULL;
		src->state2.out_delay = NULL;
	}

	/* Check the sizes are less than MAX */
	if (src->state1.fir_delay_size > MAX_FIR_DELAY_SIZE_XNCH ||
	    src->state1.out_delay_size > MAX_OUT_DELAY_SIZE_XNCH ||
	    src->state2.fir_delay_size > MAX_FIR_DELAY_SIZE_XNCH ||
	    src->state2.out_delay_size > MAX_OUT_DELAY_SIZE_XNCH) {
		src->state1.fir_delay = NULL;
		src->state1.out_delay = NULL;
		src->state2.fir_delay = NULL;
		src->state2.out_delay = NULL;
		return -EINVAL;
	}

	return 0;
}

void src_polyphase_reset(struct polyphase_src *src)
{
	src->number_of_stages = 0;
	src->stage1 = NULL;
	src->stage2 = NULL;
	src_state_reset(&src->state1);
	src_state_reset(&src->state2);
}

int src_polyphase_init(struct polyphase_src *src, struct src_param *p,
		       int32_t *delay_lines_start)
{
	struct src_stage *stage1;
	struct src_stage *stage2;
	int n_stages;
	int ret;

	if (p->idx_in < 0 || p->idx_out < 0)
		return -EINVAL;

	/* Get setup for 2 stage conversion */
	stage1 = src_table1[p->idx_out][p->idx_in];
	stage2 = src_table2[p->idx_out][p->idx_in];
	ret = init_stages(stage1, stage2, src, p, 2, delay_lines_start);
	if (ret < 0)
		return -EINVAL;

	/* Get number of stages used for optimize opportunity. 2nd
	 * stage length is one if conversion needs only one stage.
	 * If input and output rate is the same return 0 to
	 * use a simple copy function instead of 1 stage FIR with one
	 * tap.
	 */
	n_stages = (src->stage2->filter_length == 1) ? 1 : 2;
	if (p->idx_in == p->idx_out)
		n_stages = 0;

	/* If filter length for first stage is zero this is a deleted
	 * mode from in/out matrix. Computing of such SRC mode needs
	 * to be prevented.
	 */
	if (src->stage1->filter_length == 0)
		return -EINVAL;

	return n_stages;
}

/* Fallback function */
static void src_fallback(struct comp_dev *dev, struct comp_buffer *source,
			 struct comp_buffer *sink, int *n_read, int *n_written)
{
	*n_read = 0;
	*n_written = 0;
}

/* Normal 2 stage SRC */
static void src_2s(struct comp_dev *dev,
		   struct comp_buffer *source, struct comp_buffer *sink,
		   int *n_read, int *n_written)
{
	struct src_stage_prm s1;
	struct src_stage_prm s2;
	int s1_blk_in;
	int s1_blk_out;
	int s2_blk_in;
	int s2_blk_out;
	struct comp_data *cd = comp_get_drvdata(dev);
	void *sbuf_addr = cd->delay_lines;
	void *sbuf_end_addr = &cd->delay_lines[cd->param.sbuf_length];
	size_t sbuf_size = cd->param.sbuf_length * sizeof(int32_t);
	int nch = source->channels;
	int sbuf_free = cd->param.sbuf_length - cd->sbuf_avail;
	int avail_b = source->avail;
	int free_b = sink->free;
	int sz = cd->sample_container_bytes;

	*n_read = 0;
	*n_written = 0;
	s1.x_end_addr = source->end_addr;
	s1.x_size = source->size;
	s1.y_addr = sbuf_addr;
	s1.y_end_addr = sbuf_end_addr;
	s1.y_size = sbuf_size;
	s1.state = &cd->src.state1;
	s1.stage = cd->src.stage1;
	s1.x_rptr = source->r_ptr;
	s1.y_wptr = cd->sbuf_w_ptr;
	s1.nch = nch;
	s1.shift = cd->data_shift;

	s2.x_end_addr = sbuf_end_addr;
	s2.x_size = sbuf_size;
	s2.y_addr = sink->addr;
	s2.y_end_addr = sink->end_addr;
	s2.y_size = sink->size;
	s2.state = &cd->src.state2;
	s2.stage = cd->src.stage2;
	s2.x_rptr = cd->sbuf_r_ptr;
	s2.y_wptr = sink->w_ptr;
	s2.nch = nch;
	s2.shift = cd->data_shift;

	/* Test if 1st stage can be run with default block length to reach
	 * the period length or just under it.
	 */
	s1.times = cd->param.stage1_times;
	s1_blk_in = s1.times * cd->src.stage1->blk_in * nch;
	s1_blk_out = s1.times * cd->src.stage1->blk_out * nch;

	/* The sbuf may limit how many times s1 can be looped. It's harder
	 * to prepare for in advance so the repeats number is adjusted down
	 * here if need.
	 */
	if (s1_blk_out > sbuf_free) {
		s1.times = sbuf_free / (cd->src.stage1->blk_out * nch);
		s1_blk_in = s1.times * cd->src.stage1->blk_in * nch;
		s1_blk_out = s1.times * cd->src.stage1->blk_out * nch;
		tracev_src_with_ids(dev, "s1.times = %d", s1.times);
	}

	if (avail_b >= s1_blk_in * sz && sbuf_free >= s1_blk_out) {
		cd->polyphase_func(&s1);

		cd->sbuf_w_ptr = s1.y_wptr;
		cd->sbuf_avail += s1_blk_out;
		*n_read += s1.times * cd->src.stage1->blk_in;
		avail_b -= s1_blk_in * sz;
		sbuf_free -= s1_blk_out;
	}

	s2.times = cd->param.stage2_times;
	s2_blk_in = s2.times * cd->src.stage2->blk_in * nch;
	s2_blk_out = s2.times * cd->src.stage2->blk_out * nch;
	if (s2_blk_in > cd->sbuf_avail) {
		s2.times = cd->sbuf_avail / (cd->src.stage2->blk_in * nch);
		s2_blk_in = s2.times * cd->src.stage2->blk_in * nch;
		s2_blk_out = s2.times * cd->src.stage2->blk_out * nch;
		tracev_src_with_ids(dev, "s2.times = %d", s2.times);
	}

	/* Test if second stage can be run with default block length. */
	if (cd->sbuf_avail >= s2_blk_in && free_b >= s2_blk_out * sz) {
		cd->polyphase_func(&s2);

		cd->sbuf_r_ptr = s2.x_rptr;
		cd->sbuf_avail -= s2_blk_in;
		free_b -= s2_blk_out * sz;
		*n_written += s2.times * cd->src.stage2->blk_out;
	}
}

/* 1 stage SRC for simple conversions */
static void src_1s(struct comp_dev *dev,
		   struct comp_buffer *source, struct comp_buffer *sink,
		   int *n_read, int *n_written)
{
	struct src_stage_prm s1;
	struct comp_data *cd = comp_get_drvdata(dev);

	s1.times = cd->param.stage1_times;
	s1.x_rptr = source->r_ptr;
	s1.x_end_addr = source->end_addr;
	s1.x_size = source->size;
	s1.y_wptr = sink->w_ptr;
	s1.y_end_addr = sink->end_addr;
	s1.y_size = sink->size;
	s1.state = &cd->src.state1;
	s1.stage = cd->src.stage1;
	s1.nch = source->channels;
	s1.shift = cd->data_shift;

	cd->polyphase_func(&s1);

	*n_read = cd->param.blk_in;
	*n_written = cd->param.blk_out;
}

/* A fast copy function for same in and out rate */
static void src_copy_s32(struct comp_dev *dev,
			 struct comp_buffer *source, struct comp_buffer *sink,
			 int *n_read, int *n_written)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int frames = cd->param.blk_in;

	buffer_copy_s32(source, sink, frames * source->channels);

	*n_read = frames;
	*n_written = frames;
}

#if CONFIG_FORMAT_S16LE
static void src_copy_s16(struct comp_dev *dev,
			 struct comp_buffer *source, struct comp_buffer *sink,
			 int *n_read, int *n_written)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int frames = cd->param.blk_in;

	buffer_copy_s16(source, sink, frames * source->channels);

	*n_read = frames;
	*n_written = frames;
}
#endif /* CONFIG_FORMAT_S16LE */

static struct comp_dev *src_new(struct sof_ipc_comp *comp)
{
	struct comp_dev *dev;
	struct sof_ipc_comp_src *src;
	struct sof_ipc_comp_src *ipc_src = (struct sof_ipc_comp_src *)comp;
	struct comp_data *cd;
	int ret;

	trace_src("src_new()");

	if (IPC_IS_SIZE_INVALID(ipc_src->config)) {
		IPC_SIZE_ERROR_TRACE(TRACE_CLASS_SRC, ipc_src->config);
		return NULL;
	}

	/* validate init data - either SRC sink or source rate must be set */
	if (ipc_src->source_rate == 0 && ipc_src->sink_rate == 0) {
		trace_src_error("src_new() error: "
				"SRC sink and source rate are not set");
		return NULL;
	}

	dev = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
		      COMP_SIZE(struct sof_ipc_comp_src));
	if (!dev)
		return NULL;

	src = (struct sof_ipc_comp_src *)&dev->comp;

	ret = memcpy_s(src, sizeof(*src), ipc_src,
		       sizeof(struct sof_ipc_comp_src));
	assert(!ret);

	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd) {
		rfree(dev);
		return NULL;
	}

	comp_set_drvdata(dev, cd);

	cd->delay_lines = NULL;
	cd->src_func = src_fallback;
	cd->polyphase_func = NULL;
	src_polyphase_reset(&cd->src);

	dev->output_rate = ipc_src->sink_rate;

	dev->state = COMP_STATE_READY;
	return dev;
}

static void src_free(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_src_with_ids(dev, "src_free()");

	/* Free dynamically reserved buffers for SRC algorithm */
	if (cd->delay_lines)
		rfree(cd->delay_lines);

	rfree(cd);
	rfree(dev);
}

/* set component audio stream parameters */
static int src_params(struct comp_dev *dev,
		      struct sof_ipc_stream_params *params)
{
	struct sof_ipc_comp_src *src = COMP_GET_IPC(dev, sof_ipc_comp_src);
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sinkb;
	struct comp_buffer *sourceb;
	size_t delay_lines_size;
	int32_t *buffer_start;
	int n = 0;
	int err;

	trace_src_with_ids(dev, "src_params()");

	cd->sample_container_bytes = params->sample_container_bytes;

	/* src components will only ever have 1 source and 1 sink buffer */
	sourceb = list_first_item(&dev->bsource_list, struct comp_buffer,
				  sink_list);
	sinkb = list_first_item(&dev->bsink_list, struct comp_buffer,
				source_list);

	trace_src("src_params(): src->source_rate: %d", src->source_rate);
	trace_src("src_params(): src->sink_rate: %d", src->sink_rate);

	/* Calculate source and sink rates, one rate will come from IPC new
	 * and the other from params.
	 */
	if (src->source_rate == 0) {
		/* params rate is source rate */
		cd->source_rate = sourceb->rate;
		cd->sink_rate = src->sink_rate;
		/* re-write our params with output rate for next component */
		sinkb->rate = cd->sink_rate;
	} else {
		/* params rate is sink rate */
		cd->source_rate = src->source_rate;
		cd->sink_rate = sinkb->rate;
		/* re-write our params with output rate for next component */
		sourceb->rate = cd->source_rate;
	}

	cd->source_frames = dev->frames * cd->source_rate /
		cd->sink_rate;
	cd->sink_frames = dev->frames;

	/* Allocate needed memory for delay lines */
	trace_src_with_ids(dev,
			   "src_params(), source_rate = %u, sink_rate = %u",
			   cd->source_rate, cd->sink_rate);
	trace_src_with_ids(dev,
			   "src_params(), sourceb->channels = %u, sinkb->channels = %u, dev->frames = %u",
			   sourceb->channels, sinkb->channels, dev->frames);
	err = src_buffer_lengths(&cd->param, cd->source_rate, cd->sink_rate,
				 sourceb->channels, cd->source_frames);
	if (err < 0) {
		trace_src_error_with_ids(dev, "src_params() error: "
					 "src_buffer_lengths() failed");
		return err;
	}

	trace_src_with_ids(dev, "src_params(), blk_in = %u, blk_out = %u",
			   cd->param.blk_in, cd->param.blk_out);

	delay_lines_size = sizeof(int32_t) * cd->param.total;
	if (delay_lines_size == 0) {
		trace_src_error_with_ids(dev, "src_params() error: "
					 "delay_lines_size = 0");
		return -EINVAL;
	}

	/* free any existing delay lines. TODO reuse if same size */
	if (cd->delay_lines)
		rfree(cd->delay_lines);

	cd->delay_lines = rballoc(0, SOF_MEM_CAPS_RAM, delay_lines_size);
	if (!cd->delay_lines) {
		trace_src_error_with_ids(dev, "src_params() error: "
					 "failed to alloc cd->delay_lines, "
					 "delay_lines_size = %u",
					 delay_lines_size);
		return -EINVAL;
	}

	/* Clear all delay lines here */
	memset(cd->delay_lines, 0, delay_lines_size);
	buffer_start = cd->delay_lines + cd->param.sbuf_length;

	/* Initialize SRC for actual sample rate */
	n = src_polyphase_init(&cd->src, &cd->param, buffer_start);

	/* Reset stage buffer */
	cd->sbuf_r_ptr = cd->delay_lines;
	cd->sbuf_w_ptr = cd->delay_lines;
	cd->sbuf_avail = 0;

	switch (n) {
	case 0:
		/* 1:1 fast copy */
		cd->src_func = src_copy_s32;
		break;
	case 1:
		cd->src_func = src_1s; /* Simpler 1 stage SRC */
		break;
	case 2:
		cd->src_func = src_2s; /* Default 2 stage SRC */
		break;
	default:
		/* This is possibly due to missing coefficients for
		 * requested rates combination.
		 */
		trace_src_with_ids(dev, "src_params(), missing coefficients "
				   "for requested rates combination");
		cd->src_func = src_fallback;
		return -EINVAL;
	}

	return 0;
}

static int src_ctrl_cmd(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata)
{
	trace_src_error_with_ids(dev, "src_ctrl_cmd()");
	return -EINVAL;
}

/* used to pass standard and bespoke commands (with data) to component */
static int src_cmd(struct comp_dev *dev, int cmd, void *data,
		   int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = data;
	int ret = 0;

	trace_src_with_ids(dev, "src_cmd()");

	if (cmd == COMP_CMD_SET_VALUE)
		ret = src_ctrl_cmd(dev, cdata);

	return ret;
}

static int src_trigger(struct comp_dev *dev, int cmd)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_src_with_ids(dev, "src_trigger()");

	if (cmd == COMP_TRIGGER_START || cmd == COMP_TRIGGER_RELEASE)
		assert(cd->polyphase_func);

	return comp_set_state(dev, cmd);
}

static int src_get_copy_limits(struct comp_data *cd,
			       struct comp_buffer *source,
			       struct comp_buffer *sink)
{
	struct src_param *sp;
	struct src_stage *s1;
	struct src_stage *s2;
	int frames_src;
	int frames_snk;

	/* Get SRC parameters */
	sp = &cd->param;
	s1 = cd->src.stage1;
	s2 = cd->src.stage2;

	/* Calculate how many blocks can be processed with
	 * available source and free sink frames amount.
	 */
	if (s2->filter_length > 1) {
		/* Two polyphase filters case */
		frames_snk = sink->free / buffer_frame_bytes(sink);
		frames_snk = MIN(frames_snk, cd->sink_frames + s2->blk_out);
		sp->stage2_times = frames_snk / s2->blk_out;
		frames_src = source->avail / buffer_frame_bytes(source);
		frames_src = MIN(frames_src, cd->source_frames + s1->blk_in);
		sp->stage1_times = frames_src / s1->blk_in;
		sp->blk_in = sp->stage1_times * s1->blk_in;
		sp->blk_out = sp->stage2_times * s2->blk_out;
	} else {
		/* Single polyphase filter case */
		frames_snk = sink->free / buffer_frame_bytes(sink);
		frames_snk = MIN(frames_snk, cd->sink_frames + s1->blk_out);
		sp->stage1_times = frames_snk / s1->blk_out;
		frames_src = source->avail / buffer_frame_bytes(source);
		frames_snk = MIN(frames_src, cd->source_frames + s1->blk_in);
		sp->stage1_times = MIN(sp->stage1_times,
				       frames_src / s1->blk_in);
		sp->blk_in = sp->stage1_times * s1->blk_in;
		sp->blk_out = sp->stage1_times * s1->blk_out;
	}

	if (sp->blk_in == 0 || sp->blk_out == 0)
		return -EIO;

	return 0;
}

/* copy and process stream data from source to sink buffers */
static int src_copy(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *source;
	struct comp_buffer *sink;
	int ret;
	int consumed = 0;
	int produced = 0;

	tracev_src_with_ids(dev, "src_copy()");

	/* src component needs 1 source and 1 sink buffer */
	source = list_first_item(&dev->bsource_list, struct comp_buffer,
				 sink_list);
	sink = list_first_item(&dev->bsink_list, struct comp_buffer,
			       source_list);

	/* Get from buffers and SRC conversion specific block constraints
	 * how many frames can be processed. If sufficient number of samples
	 * is not available the processing is omitted.
	 */
	ret = src_get_copy_limits(cd, source, sink);
	if (ret) {
		trace_src_with_ids(dev, "No data to process.");
		return PPL_STATUS_PATH_STOP;
	}

	cd->src_func(dev, source, sink, &consumed, &produced);

	tracev_src_with_ids(dev, "src_copy(), consumed = %u,  produced = %u",
			    consumed, produced);

	/* Calc new free and available if data was processed. These
	 * functions must not be called with 0 consumed/produced.
	 */
	if (consumed > 0)
		comp_update_buffer_consume(source, consumed *
					   buffer_frame_bytes(source));

	if (produced > 0)
		comp_update_buffer_produce(sink, produced *
					   buffer_frame_bytes(sink));

	/* produced no data */
	return 0;
}

static int src_prepare(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct sof_ipc_comp_config *config = COMP_GET_CONFIG(dev);
	struct comp_buffer *sinkb;
	struct comp_buffer *sourceb;
	uint32_t source_period_bytes;
	uint32_t sink_period_bytes;
	int ret;

	trace_src_with_ids(dev, "src_prepare()");

	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	/* SRC component will only ever have 1 source and 1 sink buffer */
	sourceb = list_first_item(&dev->bsource_list,
				  struct comp_buffer, sink_list);
	sinkb = list_first_item(&dev->bsink_list,
				struct comp_buffer, source_list);

	/* get source data format and period bytes */
	cd->source_format = sourceb->frame_fmt;
	source_period_bytes = buffer_period_bytes(sourceb, dev->frames);

	/* get sink data format and period bytes */
	cd->sink_format = sinkb->frame_fmt;
	sink_period_bytes = buffer_period_bytes(sinkb, dev->frames);

	if (sinkb->size < config->periods_sink * sink_period_bytes) {
		trace_src_error_with_ids(dev, "src_prepare() error: "
					  "sink buffer size is insufficient");
		ret = -ENOMEM;
		goto err;
	}

	/* validate */
	if (!sink_period_bytes) {
		trace_src_error_with_ids(dev, "src_prepare() error: "
					 "sink_period_bytes = 0");
		ret = -EINVAL;
		goto err;
	}
	if (!source_period_bytes) {
		trace_src_error_with_ids(dev, "src_prepare() error: "
					 "source_period_bytes = 0");
		ret = -EINVAL;
		goto err;
	}

	/* SRC supports S16_LE, S24_4LE and S32_LE formats */
	if (cd->source_format != cd->sink_format) {
		trace_src_error("src_prepare() error: "
				"Source fmt %d and sink fmt %d are different.",
				cd->source_format, cd->sink_format);
		ret = -EINVAL;
		goto err;
	}

	switch (cd->source_format) {
#if CONFIG_FORMAT_S16LE
	case SOF_IPC_FRAME_S16_LE:
		cd->data_shift = 0;
		cd->polyphase_func = src_polyphase_stage_cir_s16;
		/* Copy function is set by default in params() for 32 bit
		 * data. Change it to 16 bit version here if source and sink
		 * rates are equal.
		 */
		if (cd->source_rate == cd->sink_rate)
			cd->src_func = src_copy_s16;
		break;
#endif /* CONFIG_FORMAT_S16LE */
#if CONFIG_FORMAT_S24LE
	case SOF_IPC_FRAME_S24_4LE:
		cd->data_shift = 8;
		cd->polyphase_func = src_polyphase_stage_cir;
		break;
#endif /* CONFIG_FORMAT_S24LE */
#if CONFIG_FORMAT_S32LE
	case SOF_IPC_FRAME_S32_LE:
		cd->data_shift = 0;
		cd->polyphase_func = src_polyphase_stage_cir;
		break;
#endif /* CONFIG_FORMAT_S32LE */
	default:
		trace_src_error("src_prepare() error: invalid format %d",
				cd->source_format);
		ret = -EINVAL;
		goto err;
	}

	return 0;

err:
	comp_set_state(dev, COMP_TRIGGER_RESET);
	return ret;
}

static int src_reset(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	trace_src_with_ids(dev, "src_reset()");

	cd->src_func = src_fallback;
	src_polyphase_reset(&cd->src);

	comp_set_state(dev, COMP_TRIGGER_RESET);
	return 0;
}

static const struct comp_driver comp_src = {
	.type = SOF_COMP_SRC,
	.ops = {
		.new = src_new,
		.free = src_free,
		.params = src_params,
		.cmd = src_cmd,
		.trigger = src_trigger,
		.copy = src_copy,
		.prepare = src_prepare,
		.reset = src_reset,
	},
};

static SHARED_DATA struct comp_driver_info comp_src_info = {
	.drv = &comp_src,
};

static void sys_comp_src_init(void)
{
	comp_register(platform_shared_get(&comp_src_info,
					  sizeof(comp_src_info)));
}

DECLARE_MODULE(sys_comp_src_init);
