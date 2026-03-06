/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2026, Advanced Micro Devices, Inc.
 */

#ifndef _AIE4_MSG_H_
#define _AIE4_MSG_H_

#include "amdxdna_pci_drv.h"
#include "amdxdna_ctx.h"
#include "aie_message.h"

/* aie4_message.c */
int aie4_send_msg_wait(struct amdxdna_dev_hdl *ndev, struct xdna_mailbox_msg *msg);

#endif /* _AIE4_MSG_H_ */
