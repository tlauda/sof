// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>

#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <sof/drivers/idc.h>
#include <sof/drivers/interrupt.h>
#include <sof/drivers/ipc.h>
#include <sof/drivers/timer.h>
#include <sof/lib/alloc.h>
#include <sof/lib/cache.h>
#include <sof/lib/clk.h>
#include <sof/lib/cpu.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/lib/shim.h>
#include <sof/platform.h>
#include <sof/schedule/edf_schedule.h>
#include <sof/schedule/schedule.h>
#include <sof/schedule/task.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <xtos-structs.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static SHARED_DATA struct idc_payload payload[PLATFORM_CORE_COUNT];

/**
 * \brief Returns IDC data.
 * \return Pointer to pointer of IDC data.
 */
static struct idc **idc_get(void)
{
	struct core_context *ctx = (struct core_context *)cpu_read_threadptr();

	return &ctx->idc;
}

/**
 * \brief Enables IDC interrupts.
 * \param[in] target_core Target core id.
 * \param[in] source_core Source core id.
 */
void idc_enable_interrupts(int target_core, int source_core)
{
	struct idc *idc = *idc_get();

	idc_write(IPC_IDCCTL, target_core,
		  IPC_IDCCTL_IDCTBIE(source_core));
	interrupt_unmask(idc->irq, target_core);
}

/**
 * \brief IDC interrupt handler.
 * \param[in,out] arg Pointer to IDC data.
 */
static void idc_irq_handler(void *arg)
{
	struct idc *idc = arg;
	int core = cpu_get_id();
	uint32_t idctfc;
	uint32_t idctefc;
	uint32_t idcietc;
	uint32_t i;

	tracev_idc("idc_irq_handler()");

	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		/* skip current core */
		if (core == i)
			continue;

		idctfc = idc_read(IPC_IDCTFC(i), core);

		if (idctfc & IPC_IDCTFC_BUSY) {
			trace_idc("idc_irq_handler(), IPC_IDCTFC_BUSY");

			/* disable BUSY interrupt */
			idc_write(IPC_IDCCTL, core, idc->done_bit_mask);

			idc->received_msg.core = i;
			idc->received_msg.header =
					idctfc & IPC_IDCTFC_MSG_MASK;

			idctefc = idc_read(IPC_IDCTEFC(i), core);
			idc->received_msg.extension =
					idctefc & IPC_IDCTEFC_MSG_MASK;

			schedule_task(&idc->idc_task, 0, IDC_DEADLINE);
		}
	}

	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		/* skip current core */
		if (core == i)
			continue;

		idcietc = idc_read(IPC_IDCIETC(i), core);

		if (idcietc & IPC_IDCIETC_DONE) {
			tracev_idc("idc_irq_handler(), "
				   "IPC_IDCIETC_DONE");

			idc_write(IPC_IDCIETC(i), core,
				  idcietc | IPC_IDCIETC_DONE);

			idc->msg_processed[i] = true;
		}
	}
}

/**
 * \brief Sends IDC message.
 * \param[in,out] msg Pointer to IDC message.
 * \param[in] mode Is message blocking or not.
 * \return Error code.
 */
int idc_send_msg(struct idc_msg *msg, uint32_t mode)
{
	struct timer *timer = timer_get();
	struct idc *idc = *idc_get();
	struct idc_payload *buff = idc->payload + msg->core;
	int core = cpu_get_id();
	uint64_t deadline;

	tracev_idc("arch_idc_send_msg()");

	idc->msg_processed[msg->core] = false;

	if (msg->payload)
		memcpy_s(buff, MAX_PAYLOAD_SIZE, msg->payload, msg->size);

	idc_write(IPC_IDCIETC(msg->core), core, msg->extension);
	idc_write(IPC_IDCITC(msg->core), core, msg->header | IPC_IDCITC_BUSY);

	if (mode == IDC_BLOCKING) {
		deadline = platform_timer_get(timer) +
			clock_ms_to_ticks(PLATFORM_DEFAULT_CLOCK, 1) *
			IDC_TIMEOUT / 1000;

		while (!idc->msg_processed[msg->core]) {
			if (deadline < platform_timer_get(timer)) {
				/* safe check in case we've got preempted
				 * after read
				 */
				if (idc->msg_processed[msg->core])
					return 0;

				trace_idc_error("arch_idc_send_msg() error: "
						"timeout");
				return -ETIME;
			}
		}
	}

	return 0;
}

/**
 * \brief Executes IDC IPC processing message.
 */
static void idc_ipc(void)
{
	struct ipc *ipc = ipc_get();
	struct sof_ipc_cmd_hdr *hdr = ipc->comp_data;

	ipc_cmd(hdr);
}

static void idc_params(uint32_t comp_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *dev;
	struct idc *idc = *idc_get();
	struct idc_payload *payload = idc->payload + cpu_get_id();
	struct sof_ipc_stream_params *params =
		(struct sof_ipc_stream_params *)payload;

	dev = ipc_get_comp_by_id(ipc, comp_id);
	if (!dev)
		return;

	comp_params(dev->cd, params);

	platform_shared_commit(payload, sizeof(*payload));
	platform_shared_commit(dev, sizeof(*dev));
	platform_shared_commit(ipc, sizeof(*ipc));
}

static void idc_prepare(uint32_t comp_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *dev;

	dev = ipc_get_comp_by_id(ipc, comp_id);
	if (!dev)
		return;

	comp_prepare(dev->cd);

	platform_shared_commit(dev, sizeof(*dev));
	platform_shared_commit(ipc, sizeof(*ipc));
}

static void idc_trigger(uint32_t comp_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *dev;
	struct idc *idc = *idc_get();
	struct idc_payload *payload = idc->payload + cpu_get_id();
	uint32_t *cmd = (uint32_t *)payload;

	dev = ipc_get_comp_by_id(ipc, comp_id);
	if (!dev)
		return;

	comp_trigger(dev->cd, *cmd);

	switch (*cmd) {
	case COMP_TRIGGER_START:
	case COMP_TRIGGER_RELEASE:
		schedule_task(dev->cd->task, 0, 1000);
		break;
	case COMP_TRIGGER_XRUN:
	case COMP_TRIGGER_PAUSE:
	case COMP_TRIGGER_STOP:
		schedule_task_cancel(dev->cd->task);
		break;
	}

	platform_shared_commit(payload, sizeof(*payload));
	platform_shared_commit(dev, sizeof(*dev));
	platform_shared_commit(ipc, sizeof(*ipc));
}

static void idc_reset(uint32_t comp_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *dev;

	dev = ipc_get_comp_by_id(ipc, comp_id);
	if (!dev)
		return;

	comp_reset(dev->cd);

	platform_shared_commit(dev, sizeof(*dev));
	platform_shared_commit(ipc, sizeof(*ipc));
}

/**
 * \brief Executes IDC message based on type.
 * \param[in,out] msg Pointer to IDC message.
 */
static void idc_cmd(struct idc_msg *msg)
{
	uint32_t type = iTS(msg->header);

	switch (type) {
	case iTS(IDC_MSG_POWER_DOWN):
		cpu_power_down_core();
		break;
	case iTS(IDC_MSG_NOTIFY):
		notifier_notify_remote();
		break;
	case iTS(IDC_MSG_IPC):
		idc_ipc();
		break;
	case iTS(IDC_MSG_PARAMS):
		idc_params(msg->extension);
		break;
	case iTS(IDC_MSG_PREPARE):
		idc_prepare(msg->extension);
		break;
	case iTS(IDC_MSG_TRIGGER):
		idc_trigger(msg->extension);
		break;
	case iTS(IDC_MSG_RESET):
		idc_reset(msg->extension);
		break;
	default:
		trace_idc_error("idc_cmd() error: invalid msg->header = %u",
				msg->header);
	}
}

/**
 * \brief Handles received IDC message.
 * \param[in,out] data Pointer to IDC data.
 */
static enum task_state idc_do_cmd(void *data)
{
	struct idc *idc = data;
	int core = cpu_get_id();
	int initiator = idc->received_msg.core;

	trace_idc("idc_do_cmd()");

	idc_cmd(&idc->received_msg);

	/* clear BUSY bit */
	idc_write(IPC_IDCTFC(initiator), core,
		  idc_read(IPC_IDCTFC(initiator), core) | IPC_IDCTFC_BUSY);

	/* enable BUSY interrupt */
	idc_write(IPC_IDCCTL, core, idc->busy_bit_mask | idc->done_bit_mask);

	return SOF_TASK_STATE_COMPLETED;
}

/**
 * \brief Returns BUSY interrupt mask based on core id.
 * \param[in] core Core id.
 * \return BUSY interrupt mask.
 */
static uint32_t idc_get_busy_bit_mask(int core)
{
	uint32_t busy_mask = 0;
	int i;

	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		if (i != core)
			busy_mask |= IPC_IDCCTL_IDCTBIE(i);
	}

	return busy_mask;
}

/**
 * \brief Returns DONE interrupt mask based on core id.
 * \param[in] core Core id.
 * \return DONE interrupt mask.
 */
static uint32_t idc_get_done_bit_mask(int core)
{
	uint32_t done_mask = 0;
	int i;

	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		if (i != core)
			done_mask |= IPC_IDCCTL_IDCIDIE(i);
	}

	return done_mask;
}

/**
 * \brief Initializes IDC data and registers for interrupt.
 */
int idc_init(void)
{
	int core = cpu_get_id();
	int ret;
	struct task_ops ops = {
		.run = idc_do_cmd,
		.get_deadline = ipc_task_deadline,
	};

	trace_idc("arch_idc_init()");

	/* initialize idc data */
	struct idc **idc = idc_get();
	*idc = rzalloc(SOF_MEM_ZONE_SYS, 0, SOF_MEM_CAPS_RAM, sizeof(**idc));
	(*idc)->busy_bit_mask = idc_get_busy_bit_mask(core);
	(*idc)->done_bit_mask = idc_get_done_bit_mask(core);
	(*idc)->payload = cache_to_uncache((struct idc_payload *)payload);

	/* process task */
	schedule_task_init_edf(&(*idc)->idc_task, &ops, *idc, core, 0);

	/* configure interrupt */
	(*idc)->irq = interrupt_get_irq(PLATFORM_IDC_INTERRUPT,
					PLATFORM_IDC_INTERRUPT_NAME);
	if ((*idc)->irq < 0)
		return (*idc)->irq;
	ret = interrupt_register((*idc)->irq, idc_irq_handler, *idc);
	if (ret < 0)
		return ret;
	interrupt_enable((*idc)->irq, *idc);

	/* enable BUSY and DONE interrupts */
	idc_write(IPC_IDCCTL, core,
		  (*idc)->busy_bit_mask | (*idc)->done_bit_mask);

	return 0;
}

/**
 * \brief Frees IDC data and unregisters interrupt.
 */
void idc_free(void)
{
	struct idc *idc = *idc_get();
	int core = cpu_get_id();
	int i = 0;
	uint32_t idctfc;

	trace_idc("idc_free()");

	/* disable and unregister interrupt */
	interrupt_disable(idc->irq, idc);
	interrupt_unregister(idc->irq, idc);

	/* clear BUSY bits */
	for (i = 0; i < PLATFORM_CORE_COUNT; i++) {
		idctfc = idc_read(IPC_IDCTFC(i), core);
		if (idctfc & IPC_IDCTFC_BUSY)
			idc_write(IPC_IDCTFC(i), core, idctfc);
	}

	schedule_task_free(&idc->idc_task);
}
