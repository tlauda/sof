// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2017 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>
//         Rander Wang <rander.wang@intel.com>

#include <sof/drivers/ipc.h>
#include <sof/drivers/spi.h>
#include <sof/lib/mailbox.h>
#include <sof/lib/wait.h>
#include <sof/list.h>
#include <sof/schedule/schedule.h>
#include <sof/schedule/task.h>
#include <sof/spinlock.h>
#include <ipc/header.h>
#include <stddef.h>
#include <stdint.h>

extern struct ipc *_ipc;

/* No private data for IPC */
static enum task_state ipc_platform_do_cmd(void *data)
{
	struct ipc *ipc = data;
	struct sof_ipc_cmd_hdr *hdr;
	struct sof_ipc_reply reply;

	/* perform command */
	hdr = mailbox_validate();
	ipc_cmd(hdr);

	mailbox_hostbox_read(&reply, SOF_IPC_MSG_MAX_SIZE,
			     0, sizeof(reply));
	spi_push(spi_get(SOF_SPI_INTEL_SLAVE), &reply, sizeof(reply));

	// TODO: signal audio work to enter D3 in normal context
	/* are we about to enter D3 ? */
	if (ipc->pm_prepare_D3) {
		while (1)
			wait_for_interrupt(0);
	}

	return SOF_TASK_STATE_COMPLETED;
}

void ipc_platform_send_msg(struct ipc *ipc)
{
	struct ipc_msg *msg;
	uint32_t flags;

	spin_lock_irq(ipc->lock, flags);

	/* any messages to send ? */
	if (list_is_empty(&ipc->shared_ctx->msg_list)) {
		ipc->shared_ctx->dsp_pending = 0;
		goto out;
	}

	/* now send the message */
	msg = list_first_item(&ipc->shared_ctx->msg_list, struct ipc_msg,
			      list);
	mailbox_dspbox_write(0, msg->tx_data, msg->tx_size);
	list_item_del(&msg->list);
	ipc->shared_ctx->dsp_msg = msg;
	tracev_ipc("ipc: msg tx -> 0x%x", msg->header);

	/* now interrupt host to tell it we have message sent */

	list_item_append(&msg->list, &ipc->shared_ctx->empty_list);

out:
	spin_unlock_irq(ipc->lock, flags);
}

int platform_ipc_init(struct ipc *ipc)
{
	struct task_ops ops = { .run = ipc_platform_do_cmd, .complete = NULL };

	_ipc = ipc;

	ipc_set_drvdata(_ipc, NULL);

	/* schedule */
	schedule_task_init(&_ipc->ipc_task, SOF_SCHEDULE_EDF, SOF_TASK_PRI_MED,
			   &ops, _ipc, 0, 0);

	return 0;
}
