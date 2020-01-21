// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

/*
 * IPC (InterProcessor Communication) provides a method of two way
 * communication between the host processor and the DSP. The IPC used here
 * utilises a shared mailbox and door bell between the host and DSP.
 *
 */

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <sof/debug/gdb/gdb.h>
#include <sof/debug/panic.h>
#include <sof/drivers/idc.h>
#include <sof/drivers/interrupt.h>
#include <sof/drivers/ipc.h>
#include <sof/drivers/timer.h>
#include <sof/lib/alloc.h>
#include <sof/lib/cache.h>
#include <sof/lib/cpu.h>
#include <sof/lib/dai.h>
#include <sof/lib/dma.h>
#include <sof/lib/mailbox.h>
#include <sof/lib/memory.h>
#include <sof/lib/pm_runtime.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <sof/platform.h>
#include <sof/schedule/schedule.h>
#include <sof/schedule/task.h>
#include <sof/spinlock.h>
#include <sof/string.h>
#include <sof/trace/dma-trace.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/dai.h>
#include <ipc/header.h>
#include <ipc/pm.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <ipc/trace.h>
#include <user/trace.h>
#include <config.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define iGS(x) ((x) & SOF_GLB_TYPE_MASK)
#define iCS(x) ((x) & SOF_CMD_TYPE_MASK)

/*
 * IPC ABI version compatibility rules :-
 *
 * 1) FW binaries will only support one MAJOR ABI version which is advertised
 *    to host at FW boot.
 *
 * 2) Host drivers will support the current and older MAJOR ABI versions of
 *    the IPC ABI (up to a certain age to be determined by market information).
 *
 * 3) MINOR and PATCH ABI versions can differ between host and FW but must be
 *    backwards compatible on both host and FW.
 *
 *    IPC messages sizes can be different for sender and receiver if MINOR or
 *    PATCH ABI versions differ as new fields can be added to the end of
 *    messages.
 *
 *    i) Sender > receiver: receiver only copies it's own ABI structure size.
 *
 *    ii) Receiver > sender: receiver copies its's own ABI size and zero pads
 *                           new fields. i.e. new structure fields must be non
 *                           zero to be activated.
 *
 *    Guidelines for extending ABI compatible messages :-
 *
 *    i) Use reserved fields.
 *    ii) Grow structure at the end.
 *    iii) Iff (i) and (ii) are not possible then MAJOR ABI is bumped.
 */

#define _IPC_COPY_CMD(rx, tx, rx_size)					\
	do {								\
		int ___ret;						\
		if (rx_size > tx->size) {				\
			___ret = memcpy_s(rx, rx_size, tx, tx->size);	\
			assert(!___ret);				\
			bzero((char *)rx + tx->size, rx_size - tx->size);\
			trace_ipc("ipc: hdr 0x%x rx (%d) > tx (%d)",	\
				  rx->cmd, rx_size, tx->size);		\
		} else if (tx->size > rx_size) {			\
			___ret = memcpy_s(rx, rx_size, tx, rx_size);	\
			assert(!___ret);				\
			trace_ipc("ipc: hdr 0x%x tx (%d) > rx (%d)",	\
				  rx->cmd, tx->size, rx_size);		\
		} else	{						\
			___ret = memcpy_s(rx, rx_size, tx, rx_size);	\
			assert(!___ret);				\
		}							\
	} while (0)

/* copies whole message from Tx to Rx, follows above ABI rules */
#define IPC_COPY_CMD(rx, tx) \
	_IPC_COPY_CMD(((struct sof_ipc_cmd_hdr *)&rx),			\
			((struct sof_ipc_cmd_hdr *)tx),			\
			sizeof(rx))

struct sof_ipc_cmd_hdr *mailbox_validate(void)
{
	struct sof_ipc_cmd_hdr *hdr = ipc_get()->comp_data;

	/* read component values from the inbox */
	mailbox_hostbox_read(hdr, SOF_IPC_MSG_MAX_SIZE, 0, sizeof(*hdr));

	/* validate component header */
	if (hdr->size > SOF_IPC_MSG_MAX_SIZE) {
		trace_ipc_error("ipc: msg too big at 0x%x", hdr->size);
		return NULL;
	}

	/* read rest of component data */
	mailbox_hostbox_read(hdr + 1, SOF_IPC_MSG_MAX_SIZE - sizeof(*hdr),
			     sizeof(*hdr), hdr->size - sizeof(*hdr));

	platform_shared_commit(hdr, hdr->size);

	return hdr;
}

/*
 * Stream IPC Operations.
 */

#if CONFIG_HOST_PTABLE
/* check if a pipeline is hostless when walking downstream */
static bool is_hostless_downstream(struct comp_dev *current)
{
	struct list_item *clist;

	/* check if current is a HOST comp */
	if (current->comp.type == SOF_COMP_HOST ||
	    current->comp.type == SOF_COMP_SG_HOST)
		return false;

	/* check if the pipeline has a HOST comp downstream */
	list_for_item(clist, &current->bsink_list) {
		struct comp_buffer *buffer;

		buffer = container_of(clist, struct comp_buffer, source_list);

		/* don't go downstream if this component is not connected */
		if (!buffer->sink)
			continue;

		/* dont go downstream if this comp belongs to another pipe */
		if (buffer->sink->comp.pipeline_id != current->comp.pipeline_id)
			continue;

		/* return if there's a host comp downstream */
		if (!is_hostless_downstream(buffer->sink))
			return false;
	}

	return true;
}

/* check if a pipeline is hostless when walking upstream */
static bool is_hostless_upstream(struct comp_dev *current)
{
	struct list_item *clist;

	/* check if current is a HOST comp */
	if (current->comp.type == SOF_COMP_HOST ||
	    current->comp.type == SOF_COMP_SG_HOST)
		return false;

	/* check if the pipeline has a HOST comp upstream */
	list_for_item(clist, &current->bsource_list) {
		struct comp_buffer *buffer;

		buffer = container_of(clist, struct comp_buffer, sink_list);

		/* don't go upstream if this component is not connected */
		if (!buffer->source)
			continue;

		/* dont go upstream if this comp belongs to another pipeline */
		if (buffer->source->comp.pipeline_id !=
		    current->comp.pipeline_id)
			continue;

		/* return if there is a host comp upstream */
		if (!is_hostless_upstream(buffer->source))
			return false;
	}

	return true;
}
#endif

/* allocate a new stream */
static int ipc_stream_pcm_params(uint32_t stream)
{
#if CONFIG_HOST_PTABLE
	struct sof_ipc_comp_host *host = NULL;
	struct dma_sg_elem_array elem_array;
	uint32_t ring_size;
	enum comp_copy_type copy_type = COMP_COPY_ONE_SHOT;
	struct comp_dev *cd;
#endif
	struct ipc *ipc = ipc_get();
	struct sof_ipc_pcm_params pcm_params;
	struct sof_ipc_pcm_params_reply reply;
	struct ipc_comp_dev *pcm_dev;
	int err, reset_err;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(pcm_params, ipc->comp_data);

	trace_ipc("ipc: comp %d -> params", pcm_params.comp_id);

	/* get the pcm_dev */
	pcm_dev = ipc_get_comp_by_id(ipc, pcm_params.comp_id);
	if (pcm_dev == NULL) {
		trace_ipc_error("ipc: comp %d not found", pcm_params.comp_id);
		return -ENODEV;
	}

	/* sanity check comp */
	if (pcm_dev->cd->pipeline == NULL) {
		trace_ipc_error("ipc: comp %d pipeline not found",
				pcm_params.comp_id);
		return -EINVAL;
	}

	if (IPC_IS_SIZE_INVALID(pcm_params.params)) {
		IPC_SIZE_ERROR_TRACE(TRACE_CLASS_IPC, pcm_params.params);
		return -EINVAL;
	}

#if CONFIG_HOST_PTABLE
	cd = pcm_dev->cd;

	/*
	 * walk in both directions to check if the pipeline is hostless
	 * skip page table set up if it is
	 */
	if (is_hostless_downstream(cd) && is_hostless_upstream(cd))
		goto pipe_params;

	/* Parse host tables */
	host = (struct sof_ipc_comp_host *)&cd->comp;
	if (IPC_IS_SIZE_INVALID(host->config)) {
		IPC_SIZE_ERROR_TRACE(TRACE_CLASS_IPC, host->config);
		err = -EINVAL;
		goto error;
	}

	err = ipc_process_host_buffer(ipc, &pcm_params.params.buffer,
				      host->direction,
				      &elem_array,
				      &ring_size);
	if (err < 0)
		goto error;

	err = comp_set_attribute(cd, COMP_ATTR_HOST_BUFFER, &elem_array);
	if (err < 0) {
		trace_ipc_error("ipc: comp %d host buffer failed %d",
				pcm_params.comp_id, err);
		goto error;
	}

	/* TODO: should be extracted to platform specific code */
	err = comp_set_attribute(cd, COMP_ATTR_COPY_TYPE, &copy_type);
	if (err < 0) {
		trace_ipc_error("ipc: comp %d setting copy type failed %d",
				pcm_params.comp_id, err);
		goto error;
	}

pipe_params:
#endif

	/* configure pipeline audio params */
	err = pipeline_params(pcm_dev->cd->pipeline, pcm_dev->cd,
			(struct sof_ipc_pcm_params *)ipc_get()->comp_data);
	if (err < 0) {
		trace_ipc_error("ipc: pipe %d comp %d params failed %d",
				pcm_dev->cd->pipeline->ipc_pipe.pipeline_id,
				pcm_params.comp_id, err);
		goto error;
	}

	/* prepare pipeline audio params */
	err = pipeline_prepare(pcm_dev->cd->pipeline, pcm_dev->cd);
	if (err < 0) {
		trace_ipc_error("ipc: pipe %d comp %d prepare failed %d",
				pcm_dev->cd->pipeline->ipc_pipe.pipeline_id,
				pcm_params.comp_id, err);
		goto error;
	}

	/* write component values to the outbox */
	reply.rhdr.hdr.size = sizeof(reply);
	reply.rhdr.hdr.cmd = stream;
	reply.rhdr.error = 0;
	reply.comp_id = pcm_params.comp_id;
	reply.posn_offset = pcm_dev->cd->pipeline->posn_offset;
	mailbox_hostbox_write(0, &reply, sizeof(reply));
	platform_shared_commit(pcm_dev, sizeof(*pcm_dev));
	return 1;

error:
	reset_err = pipeline_reset(pcm_dev->cd->pipeline, pcm_dev->cd);
	if (reset_err < 0)
		trace_ipc_error("ipc: pipe %d comp %d reset failed %d",
				pcm_dev->cd->pipeline->ipc_pipe.pipeline_id,
				pcm_params.comp_id, reset_err);
	platform_shared_commit(pcm_dev, sizeof(*pcm_dev));
	return err;
}

/* free stream resources */
static int ipc_stream_pcm_free(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_stream free_req;
	struct ipc_comp_dev *pcm_dev;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(free_req, ipc->comp_data);

	trace_ipc("ipc: comp %d -> free", free_req.comp_id);

	/* get the pcm_dev */
	pcm_dev = ipc_get_comp_by_id(ipc, free_req.comp_id);
	if (pcm_dev == NULL) {
		trace_ipc_error("ipc: comp %d not found", free_req.comp_id);
		return -ENODEV;
	}

	/* sanity check comp */
	if (pcm_dev->cd->pipeline == NULL) {
		trace_ipc_error("ipc: comp %d pipeline not found",
				free_req.comp_id);
		return -EINVAL;
	}

	/* reset the pipeline */
	ret = pipeline_reset(pcm_dev->cd->pipeline, pcm_dev->cd);

	platform_shared_commit(pcm_dev, sizeof(*pcm_dev));

	return ret;
}

/* get stream position */
static int ipc_stream_position(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_stream stream;
	struct sof_ipc_stream_posn posn;
	struct ipc_comp_dev *pcm_dev;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(stream, ipc->comp_data);

	trace_ipc("ipc: comp %d -> position", stream.comp_id);

	memset(&posn, 0, sizeof(posn));

	/* get the pcm_dev */
	pcm_dev = ipc_get_comp_by_id(ipc, stream.comp_id);
	if (pcm_dev == NULL) {
		trace_ipc_error("ipc: comp %d not found", stream.comp_id);
		return -ENODEV;
	}

	/* set message fields - TODO; get others */
	posn.rhdr.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_POSITION |
			    stream.comp_id;
	posn.rhdr.hdr.size = sizeof(posn);
	posn.comp_id = stream.comp_id;

	/* get the stream positions and timestamps */
	pipeline_get_timestamp(pcm_dev->cd->pipeline, pcm_dev->cd, &posn);

	/* copy positions to stream region */
	mailbox_stream_write(pcm_dev->cd->pipeline->posn_offset,
			     &posn, sizeof(posn));

	platform_shared_commit(pcm_dev, sizeof(*pcm_dev));

	return 1;
}

/* send stream position */
int ipc_stream_send_position(struct comp_dev *cdev,
	struct sof_ipc_stream_posn *posn)
{
	posn->rhdr.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_POSITION |
		cdev->comp.id;
	posn->rhdr.hdr.size = sizeof(*posn);
	posn->comp_id = cdev->comp.id;

	mailbox_stream_write(cdev->pipeline->posn_offset, posn, sizeof(*posn));
	return ipc_queue_host_message(ipc_get(), posn->rhdr.hdr.cmd, posn,
				      sizeof(*posn), false);
}

/* send component notification */
int ipc_send_comp_notification(struct comp_dev *cdev,
			       struct sof_ipc_comp_event *event)
{
	event->rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG |
		SOF_IPC_COMP_NOTIFICATION | cdev->comp.id;
	event->rhdr.hdr.size = sizeof(*event);
	event->src_comp_type = cdev->comp.type;
	event->src_comp_id = cdev->comp.id;

	return ipc_queue_host_message(ipc_get(), event->rhdr.hdr.cmd, event,
				      sizeof(*event), false);
}

/* send stream position TODO: send compound message  */
int ipc_stream_send_xrun(struct comp_dev *cdev,
	struct sof_ipc_stream_posn *posn)
{
	posn->rhdr.hdr.cmd = SOF_IPC_GLB_STREAM_MSG |
			     SOF_IPC_STREAM_TRIG_XRUN |
			     cdev->comp.id;
	posn->rhdr.hdr.size = sizeof(*posn);
	posn->comp_id = cdev->comp.id;

	mailbox_stream_write(cdev->pipeline->posn_offset, posn, sizeof(*posn));
	return ipc_queue_host_message(ipc_get(), posn->rhdr.hdr.cmd, posn,
				      sizeof(*posn), false);
}

static int ipc_stream_trigger(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *pcm_dev;
	struct sof_ipc_stream stream;
	uint32_t ipc_cmd = iCS(header);
	uint32_t cmd;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(stream, ipc->comp_data);

	trace_ipc("ipc: comp %d -> trigger cmd 0x%x", stream.comp_id, ipc_cmd);

	/* get the pcm_dev */
	pcm_dev = ipc_get_comp_by_id(ipc, stream.comp_id);
	if (pcm_dev == NULL) {
		trace_ipc_error("ipc: comp %d not found", stream.comp_id);
		return -ENODEV;
	}

	switch (ipc_cmd) {
	case SOF_IPC_STREAM_TRIG_START:
		cmd = COMP_TRIGGER_START;
		break;
	case SOF_IPC_STREAM_TRIG_STOP:
		cmd = COMP_TRIGGER_STOP;
		break;
	case SOF_IPC_STREAM_TRIG_PAUSE:
		cmd = COMP_TRIGGER_PAUSE;
		break;
	case SOF_IPC_STREAM_TRIG_RELEASE:
		cmd = COMP_TRIGGER_RELEASE;
		break;
	/* XRUN is special case- TODO */
	case SOF_IPC_STREAM_TRIG_XRUN:
		return 0;
	default:
		trace_ipc_error("ipc: invalid trigger cmd 0x%x", ipc_cmd);
		return -ENODEV;
	}

	/* trigger the component */
	ret = pipeline_trigger(pcm_dev->cd->pipeline, pcm_dev->cd, cmd);
	if (ret < 0) {
		trace_ipc_error("ipc: comp %d trigger 0x%x failed %d",
				stream.comp_id, ipc_cmd, ret);
	}

	platform_shared_commit(pcm_dev, sizeof(*pcm_dev));

	return ret;
}

static int ipc_glb_stream_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_STREAM_PCM_PARAMS:
		return ipc_stream_pcm_params(header);
	case SOF_IPC_STREAM_PCM_FREE:
		return ipc_stream_pcm_free(header);
	case SOF_IPC_STREAM_TRIG_START:
	case SOF_IPC_STREAM_TRIG_STOP:
	case SOF_IPC_STREAM_TRIG_PAUSE:
	case SOF_IPC_STREAM_TRIG_RELEASE:
	case SOF_IPC_STREAM_TRIG_DRAIN:
	case SOF_IPC_STREAM_TRIG_XRUN:
		return ipc_stream_trigger(header);
	case SOF_IPC_STREAM_POSITION:
		return ipc_stream_position(header);
	default:
		trace_ipc_error("ipc: unknown stream cmd 0x%x", cmd);
		return -EINVAL;
	}
}

/*
 * DAI IPC Operations.
 */

static int ipc_dai_config(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_dai_config config;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(config, ipc->comp_data);

	trace_ipc("ipc: dai %d,%d -> config ", config.type,
		  config.dai_index);

	/* send params to all DAI components who use that physical DAI */
	return ipc_comp_dai_config(ipc, &config);
}

static int ipc_glb_dai_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_DAI_CONFIG:
		return ipc_dai_config(header);
	case SOF_IPC_DAI_LOOPBACK:
		//return ipc_comp_set_value(header, COMP_CMD_LOOPBACK);
	default:
		trace_ipc_error("ipc: unknown DAI cmd 0x%x", cmd);
		return -EINVAL;
	}
}

/*
 * PM IPC Operations.
 */

static int ipc_pm_context_size(uint32_t header)
{
	struct sof_ipc_pm_ctx pm_ctx;

	trace_ipc("ipc: pm -> size");

	bzero(&pm_ctx, sizeof(pm_ctx));

	/* TODO: calculate the context and size of host buffers required */

	/* write the context to the host driver */
	//mailbox_hostbox_write(0, &pm_ctx, sizeof(pm_ctx));

	return 0;
}

static int ipc_pm_context_save(uint32_t header)
{
	//struct sof_ipc_pm_ctx *pm_ctx = _ipc->comp_data;

	trace_ipc("ipc: pm -> save");

	/* TODO: check we are inactive - all streams are suspended */

	/* TODO: mask ALL platform interrupts except DMA */

	/* TODO now save the context - create SG buffer config using */
	//mm_pm_context_save(struct dma_sg_config *sg);

	/* mask all DSP interrupts */
	arch_interrupt_disable_mask(0xffffffff);

	/* TODO: mask ALL platform interrupts inc DMA */

	/* TODO: clear any outstanding platform IRQs - TODO refine */

	/* TODO: stop ALL timers */
	platform_timer_stop(timer_get());

	/* TODO: disable SSP and DMA HW */

	/* TODO: save the context */
	//reply.entries_no = 0;

	/* write the context to the host driver */
	//mailbox_hostbox_write(0, pm_ctx, sizeof(*pm_ctx));

	ipc_get()->pm_prepare_D3 = 1;

	return 0;
}

static int ipc_pm_context_restore(uint32_t header)
{
	//struct sof_ipc_pm_ctx *pm_ctx = _ipc->comp_data;

	trace_ipc("ipc: pm -> restore");

	ipc_get()->pm_prepare_D3 = 0;

	/* restore context placeholder */
	//mailbox_hostbox_write(0, pm_ctx, sizeof(*pm_ctx));

	return 0;
}

static int ipc_pm_core_enable(uint32_t header)
{
	struct sof_ipc_pm_core_config pm_core_config;
	int i = 0;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(pm_core_config, ipc_get()->comp_data);

	trace_ipc("ipc: pm core mask 0x%x -> enable",
		  pm_core_config.enable_mask);

	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		if (i != PLATFORM_MASTER_CORE_ID) {
			if (pm_core_config.enable_mask & (1 << i))
				cpu_enable_core(i);
			else
				cpu_disable_core(i);
		}
	}

	return 0;
}

static int ipc_pm_gate(uint32_t header)
{
	struct sof_ipc_pm_gate pm_gate;

	IPC_COPY_CMD(pm_gate, ipc_get()->comp_data);

	/* pause dma trace firstly if needed */
	if (pm_gate.flags & SOF_PM_NO_TRACE)
		trace_off();

	if (pm_gate.flags & SOF_PM_PPG)
		pm_runtime_disable(PM_RUNTIME_DSP, PLATFORM_MASTER_CORE_ID);
	else
		pm_runtime_enable(PM_RUNTIME_DSP, PLATFORM_MASTER_CORE_ID);

	/* resume dma trace if needed */
	if (!(pm_gate.flags & SOF_PM_NO_TRACE))
		trace_on();

	return 0;
}

static int ipc_glb_pm_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_PM_CTX_SAVE:
		return ipc_pm_context_save(header);
	case SOF_IPC_PM_CTX_RESTORE:
		return ipc_pm_context_restore(header);
	case SOF_IPC_PM_CTX_SIZE:
		return ipc_pm_context_size(header);
	case SOF_IPC_PM_CORE_ENABLE:
		return ipc_pm_core_enable(header);
	case SOF_IPC_PM_GATE:
		return ipc_pm_gate(header);
	case SOF_IPC_PM_CLK_SET:
	case SOF_IPC_PM_CLK_GET:
	case SOF_IPC_PM_CLK_REQ:
	default:
		trace_ipc_error("ipc: unknown pm cmd 0x%x", cmd);
		return -EINVAL;
	}
}

#if CONFIG_TRACE
/*
 * Debug IPC Operations.
 */
static int ipc_dma_trace_config(uint32_t header)
{
#if CONFIG_HOST_PTABLE
	struct dma_sg_elem_array elem_array;
	uint32_t ring_size;
#endif
	struct dma_trace_data *dmat = dma_trace_data_get();
	struct ipc *ipc = ipc_get();
	struct sof_ipc_dma_trace_params_ext params;
	struct timer *timer = timer_get();
	int err;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(params, ipc->comp_data);

	if (iCS(header) == SOF_IPC_TRACE_DMA_PARAMS_EXT)
		platform_timer_set_delta(timer, params.timestamp_ns);
	else
		timer->delta = 0;

	platform_shared_commit(timer, sizeof(*timer));

#if CONFIG_SUECREEK
	return 0;
#endif

#if CONFIG_HOST_PTABLE
	err = ipc_process_host_buffer(ipc, &params.buffer,
				      SOF_IPC_STREAM_CAPTURE,
				      &elem_array,
				      &ring_size);
	if (err < 0)
		goto error;

	err = dma_trace_host_buffer(dmat, &elem_array, ring_size);
	if (err < 0) {
		trace_ipc_error("ipc: trace failed to set host buffers %d",
				err);
		goto error;
	}
#else
	/* stream tag of capture stream for DMA trace */
	dmat->stream_tag = params.stream_tag;

	/* host buffer size for DMA trace */
	dmat->host_size = params.buffer.size;
#endif

	err = dma_trace_enable(dmat);
	if (err < 0) {
		trace_ipc_error("ipc: failed to enable trace %d", err);
		goto error;
	}

	return 0;

error:
	return err;
}

/* send DMA trace host buffer position to host */
int ipc_dma_trace_send_position(void)
{
	struct dma_trace_data *dmat = dma_trace_data_get();
	struct sof_ipc_dma_trace_posn posn;

	posn.rhdr.hdr.cmd =  SOF_IPC_GLB_TRACE_MSG | SOF_IPC_TRACE_DMA_POSITION;
	posn.host_offset = dmat->host_offset;
	posn.overflow = dmat->overflow;
	posn.messages = dmat->messages;
	posn.rhdr.hdr.size = sizeof(posn);

	return ipc_queue_host_message(ipc_get(), posn.rhdr.hdr.cmd, &posn,
				      sizeof(posn), true);
}

static int ipc_glb_debug_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	trace_ipc("ipc: debug cmd 0x%x", cmd);

	switch (cmd) {
	case SOF_IPC_TRACE_DMA_PARAMS:
	case SOF_IPC_TRACE_DMA_PARAMS_EXT:
		return ipc_dma_trace_config(header);
	default:
		trace_ipc_error("ipc: unknown debug cmd 0x%x", cmd);
		return -EINVAL;
	}
}
#else
static int ipc_glb_debug_message(uint32_t header)
{
	/* traces are disabled - CONFIG_TRACE is not set */

	return -EINVAL;
}
#endif

static int ipc_glb_gdb_debug(uint32_t header)
{
	/* no furher information needs to be extracted form header */
	(void) header;

#if CONFIG_GDB_DEBUG
	gdb_init_debug_exception();
	gdb_init();
	/* TODO: this asm should be in arch/include/debug/debug.h
	 * with a generic name and trigger debug exception
	 */
	asm volatile("_break 0, 0");
	return 0;
#else
	return -EINVAL;
#endif

}

/*
 * Topology IPC Operations.
 */

static int ipc_comp_cmd(struct comp_dev *dev, int cmd,
			struct sof_ipc_ctrl_data *data, int size)
{
	struct idc_msg comp_cmd_msg;

	/* pipeline running on other core */
	if (dev->pipeline && dev->pipeline->status == COMP_STATE_ACTIVE &&
	    cpu_get_id() != dev->pipeline->ipc_pipe.core) {

		/* check if requested core is enabled */
		if (!cpu_is_core_enabled(dev->pipeline->ipc_pipe.core))
			return -EINVAL;

		/* build IDC message */
		comp_cmd_msg.header = IDC_MSG_COMP_CMD;
		comp_cmd_msg.extension = IDC_MSG_COMP_CMD_EXT(cmd);
		comp_cmd_msg.core = dev->pipeline->ipc_pipe.core;

		/* send IDC component command message */
		return idc_send_msg(&comp_cmd_msg, IDC_BLOCKING);
	} else {
		return comp_cmd(dev, cmd, data, size);
	}
}

/* get/set component values or runtime data */
static int ipc_comp_value(uint32_t header, uint32_t cmd)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *comp_dev;
	struct sof_ipc_ctrl_data data, *_data = ipc->comp_data;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(data, ipc->comp_data);

	trace_ipc("ipc: comp %d -> cmd %d", data.comp_id, data.cmd);

	/* get the component */
	comp_dev = ipc_get_comp_by_id(ipc, data.comp_id);
	if (!comp_dev) {
		trace_ipc_error("ipc: comp %d not found", data.comp_id);
		return -ENODEV;
	}

	/* get component values */
	ret = ipc_comp_cmd(comp_dev->cd, cmd, _data, SOF_IPC_MSG_MAX_SIZE);
	if (ret < 0) {
		trace_ipc_error("ipc: comp %d cmd %u failed %d", data.comp_id,
				data.cmd, ret);
		return ret;
	}

	platform_shared_commit(comp_dev, sizeof(*comp_dev));

	/* write component values to the outbox */
	if (_data->rhdr.hdr.size <= MAILBOX_HOSTBOX_SIZE &&
	    _data->rhdr.hdr.size <= SOF_IPC_MSG_MAX_SIZE) {
		mailbox_hostbox_write(0, _data, data.rhdr.hdr.size);
		ret = 1;
	} else {
		trace_ipc_error("ipc: comp %d cmd %u returned %d bytes max %d",
				data.comp_id, data.cmd, _data->rhdr.hdr.size,
				MIN(MAILBOX_HOSTBOX_SIZE,
				    SOF_IPC_MSG_MAX_SIZE));
		ret = -EINVAL;
	}

	return ret;
}

static int ipc_glb_comp_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_COMP_SET_VALUE:
		return ipc_comp_value(header, COMP_CMD_SET_VALUE);
	case SOF_IPC_COMP_GET_VALUE:
		return ipc_comp_value(header, COMP_CMD_GET_VALUE);
	case SOF_IPC_COMP_SET_DATA:
		return ipc_comp_value(header, COMP_CMD_SET_DATA);
	case SOF_IPC_COMP_GET_DATA:
		return ipc_comp_value(header, COMP_CMD_GET_DATA);
	default:
		trace_ipc_error("ipc: unknown comp cmd 0x%x", cmd);
		return -EINVAL;
	}
}

static int ipc_glb_tplg_comp_new(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_comp comp;
	struct sof_ipc_comp_reply reply;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(comp, ipc->comp_data);

	trace_ipc("ipc: pipe %d comp %d -> new (type %d)", comp.pipeline_id,
		  comp.id, comp.type);

	/* register component */
	ret = ipc_comp_new(ipc, (struct sof_ipc_comp *)ipc->comp_data);
	if (ret < 0) {
		trace_ipc_error("ipc: pipe %d comp %d creation failed %d",
				comp.pipeline_id, comp.id, ret);
		return ret;
	}

	/* write component values to the outbox */
	reply.rhdr.hdr.size = sizeof(reply);
	reply.rhdr.hdr.cmd = header;
	reply.rhdr.error = 0;
	reply.offset = 0; /* TODO: set this up for mmaped components */
	mailbox_hostbox_write(0, &reply, sizeof(reply));
	return 1;
}

static int ipc_glb_tplg_buffer_new(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_buffer ipc_buffer;
	struct sof_ipc_comp_reply reply;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(ipc_buffer, ipc->comp_data);

	trace_ipc("ipc: pipe %d buffer %d -> new (0x%x bytes)",
		  ipc_buffer.comp.pipeline_id, ipc_buffer.comp.id,
		  ipc_buffer.size);

	ret = ipc_buffer_new(ipc, (struct sof_ipc_buffer *)ipc->comp_data);
	if (ret < 0) {
		trace_ipc_error("ipc: pipe %d buffer %d creation failed %d",
				ipc_buffer.comp.pipeline_id,
				ipc_buffer.comp.id, ret);
		return ret;
	}

	/* write component values to the outbox */
	reply.rhdr.hdr.size = sizeof(reply);
	reply.rhdr.hdr.cmd = header;
	reply.rhdr.error = 0;
	reply.offset = 0; /* TODO: set this up for mmaped components */
	mailbox_hostbox_write(0, &reply, sizeof(reply));
	return 1;
}

static int ipc_glb_tplg_pipe_new(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_pipe_new ipc_pipeline;
	struct sof_ipc_comp_reply reply;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(ipc_pipeline, ipc->comp_data);

	trace_ipc("ipc: pipe %d -> new", ipc_pipeline.pipeline_id);

	ret = ipc_pipeline_new(ipc,
			       (struct sof_ipc_pipe_new *)ipc->comp_data);
	if (ret < 0) {
		trace_ipc_error("ipc: pipe %d creation failed %d",
				ipc_pipeline.pipeline_id, ret);
		return ret;
	}

	/* write component values to the outbox */
	reply.rhdr.hdr.size = sizeof(reply);
	reply.rhdr.hdr.cmd = header;
	reply.rhdr.error = 0;
	reply.offset = 0; /* TODO: set this up for mmaped components */
	mailbox_hostbox_write(0, &reply, sizeof(reply));
	return 1;
}

static int ipc_glb_tplg_pipe_complete(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_pipe_ready ipc_pipeline;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(ipc_pipeline, ipc->comp_data);

	trace_ipc("ipc: pipe %d -> complete", ipc_pipeline.comp_id);

	return ipc_pipeline_complete(ipc, ipc_pipeline.comp_id);
}

static int ipc_glb_tplg_comp_connect(uint32_t header)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_pipe_comp_connect connect;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(connect, ipc->comp_data);

	trace_ipc("ipc: comp sink %d, source %d  -> connect",
		  connect.sink_id, connect.source_id);

	return ipc_comp_connect(ipc,
			(struct sof_ipc_pipe_comp_connect *)ipc->comp_data);
}

static int ipc_glb_tplg_free(uint32_t header,
		int (*free_func)(struct ipc *ipc, uint32_t id))
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_free ipc_free;
	int ret;

	/* copy message with ABI safe method */
	IPC_COPY_CMD(ipc_free, ipc->comp_data);

	trace_ipc("ipc: comp %d -> free", ipc_free.id);

	/* free the object */
	ret = free_func(ipc, ipc_free.id);

	if (ret < 0) {
		trace_ipc_error("ipc: comp %d free failed %d",
				ipc_free.id, ret);
	}

	return ret;
}

static int ipc_glb_tplg_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_TPLG_COMP_NEW:
		return ipc_glb_tplg_comp_new(header);
	case SOF_IPC_TPLG_COMP_FREE:
		return ipc_glb_tplg_free(header, ipc_comp_free);
	case SOF_IPC_TPLG_COMP_CONNECT:
		return ipc_glb_tplg_comp_connect(header);
	case SOF_IPC_TPLG_PIPE_NEW:
		return ipc_glb_tplg_pipe_new(header);
	case SOF_IPC_TPLG_PIPE_COMPLETE:
		return ipc_glb_tplg_pipe_complete(header);
	case SOF_IPC_TPLG_PIPE_FREE:
		return ipc_glb_tplg_free(header, ipc_pipeline_free);
	case SOF_IPC_TPLG_BUFFER_NEW:
		return ipc_glb_tplg_buffer_new(header);
	case SOF_IPC_TPLG_BUFFER_FREE:
		return ipc_glb_tplg_free(header, ipc_buffer_free);
	default:
		trace_ipc_error("ipc: unknown tplg header 0x%x", header);
		return -EINVAL;
	}
}

#if CONFIG_DEBUG
static int ipc_glb_test_message(uint32_t header)
{
	uint32_t cmd = iCS(header);

	switch (cmd) {
	case SOF_IPC_TEST_IPC_FLOOD:
		return 0; /* just return so next IPC can be sent */
	default:
		trace_ipc_error("ipc: unknown test header 0x%x", header);
		return -EINVAL;
	}
}
#endif

/*
 * Global IPC Operations.
 */

void ipc_cmd(struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_ipc_reply reply;
	uint32_t type = 0;
	int ret;

	if (hdr == NULL) {
		trace_ipc_error("ipc: invalid IPC header.");
		ret = -EINVAL;
		goto out;
	}

	type = iGS(hdr->cmd);

	switch (type) {
	case SOF_IPC_GLB_REPLY:
		ret = 0;
		break;
	case SOF_IPC_GLB_COMPOUND:
		ret = -EINVAL;	/* TODO */
		break;
	case SOF_IPC_GLB_TPLG_MSG:
		ret = ipc_glb_tplg_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_PM_MSG:
		ret = ipc_glb_pm_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_COMP_MSG:
		ret = ipc_glb_comp_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_STREAM_MSG:
		ret = ipc_glb_stream_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_DAI_MSG:
		ret = ipc_glb_dai_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_TRACE_MSG:
		ret = ipc_glb_debug_message(hdr->cmd);
		break;
	case SOF_IPC_GLB_GDB_DEBUG:
		ret = ipc_glb_gdb_debug(hdr->cmd);
		break;
#if CONFIG_DEBUG
	case SOF_IPC_GLB_TEST:
		ret = ipc_glb_test_message(hdr->cmd);
		break;
#endif
	default:
		trace_ipc_error("ipc: unknown command type %u", type);
		ret = -EINVAL;
		break;
	}

	platform_shared_commit(hdr, hdr->size);

out:
	tracev_ipc("ipc: last request %d returned %d", type, ret);

	/* if ret > 0, reply created and copied by cmd() */
	if (ret <= 0) {
		/* send std error/ok reply */
		reply.error = ret;

		reply.hdr.cmd = SOF_IPC_GLB_REPLY;
		reply.hdr.size = sizeof(reply);
		mailbox_hostbox_write(0, &reply, sizeof(reply));
	}
}

/* locks held by caller */
static inline struct ipc_msg *msg_get_empty(struct ipc *ipc)
{
	struct ipc_msg *msg = NULL;

	if (!list_is_empty(&ipc->empty_list)) {
		msg = list_first_item(&ipc->empty_list, struct ipc_msg, list);
		list_item_del(&msg->list);
	}

	return msg;
}

static inline struct ipc_msg *ipc_glb_stream_message_find(struct ipc *ipc,
	struct sof_ipc_stream_posn *posn)
{
	struct list_item *plist;
	struct ipc_msg *msg = NULL;
	struct sof_ipc_stream_posn *old_posn = NULL;
	uint32_t cmd;

	/* Check whether the command is expected */
	cmd = iCS(posn->rhdr.hdr.cmd);

	switch (cmd) {
	case SOF_IPC_STREAM_TRIG_XRUN:
	case SOF_IPC_STREAM_POSITION:

		/* iterate host message list for searching */
		list_for_item(plist, &ipc->msg_list) {
			msg = container_of(plist, struct ipc_msg, list);
			if (msg->header == posn->rhdr.hdr.cmd) {
				old_posn = (struct sof_ipc_stream_posn *)
					   msg->tx_data;
				if (old_posn->comp_id == posn->comp_id)
					return msg;
			}
			platform_shared_commit(msg, sizeof(*msg));
		}
		break;
	default:
		break;
	}

	/* no match */
	return NULL;
}

static inline struct ipc_msg *ipc_glb_trace_message_find(struct ipc *ipc,
	struct sof_ipc_dma_trace_posn *posn)
{
	struct list_item *plist;
	struct ipc_msg *msg = NULL;
	uint32_t cmd;

	/* Check whether the command is expected */
	cmd = iCS(posn->rhdr.hdr.cmd);

	switch (cmd) {
	case SOF_IPC_TRACE_DMA_POSITION:
		/* iterate host message list for searching */
		list_for_item(plist, &ipc->msg_list) {
			msg = container_of(plist, struct ipc_msg, list);
			if (msg->header == posn->rhdr.hdr.cmd)
				return msg;
			platform_shared_commit(msg, sizeof(*msg));
		}
		break;
	default:
		break;
	}

	/* no match */
	return NULL;
}

static inline struct ipc_msg *msg_find(struct ipc *ipc, uint32_t header,
	void *tx_data)
{
	uint32_t type;

	/* use different sub function for different global message type */
	type = iGS(header);

	switch (type) {
	case SOF_IPC_GLB_STREAM_MSG:
		return ipc_glb_stream_message_find(ipc,
			(struct sof_ipc_stream_posn *)tx_data);
	case SOF_IPC_GLB_TRACE_MSG:
		return ipc_glb_trace_message_find(ipc,
			(struct sof_ipc_dma_trace_posn *)tx_data);
	default:
		/* not found */
		return NULL;
	}
}

int ipc_queue_host_message(struct ipc *ipc, uint32_t header, void *tx_data,
			   size_t tx_bytes, bool replace)
{
	struct ipc_msg *msg = NULL;
	bool found = false;
	uint32_t flags;
	int ret = 0;

	spin_lock_irq(&ipc->lock, flags);

	/* do we need to replace an existing message? */
	if (replace)
		msg = msg_find(ipc, header, tx_data);

	/* do we need to use a new empty message? */
	if (msg)
		found = true;
	else
		msg = msg_get_empty(ipc);

	if (!msg) {
		trace_ipc_error(
			"ipc_queue_host_message() error: msg hdr for 0x%08x not found",
			header);
		ret = -EBUSY;
		goto out;
	}

	/* prepare the message */
	msg->header = header;
	msg->tx_size = tx_bytes;

	/* copy mailbox data to message */
	if (tx_bytes > 0 && tx_bytes < SOF_IPC_MSG_MAX_SIZE) {
		ret = memcpy_s(msg->tx_data, msg->tx_size, tx_data, tx_bytes);
		assert(!ret);
	}

	/* queue new message if it's not replacement */
	if (!found)
		list_item_append(&msg->list, &ipc->msg_list);

	platform_shared_commit(msg, sizeof(*msg));

out:
	spin_unlock_irq(&ipc->lock, flags);
	return ret;
}

void ipc_schedule_process(struct ipc *ipc)
{
	schedule_task(&ipc->ipc_task, 0, 100);

	platform_shared_commit(ipc, sizeof(*ipc));
}
