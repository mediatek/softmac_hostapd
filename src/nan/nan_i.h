/*
 * Wi-Fi Aware - Internal definitions for NAN module
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef NAN_I_H
#define NAN_I_H

#include "list.h"

struct nan_config;

/**
 * struct nan_peer - Represents a known NAN peer
 * @list: List node for linking peers
 * @nmi_addr: NMI of the peer
 * @last_seen: Timestamp of the last time this peer was seen
 */
struct nan_peer {
	struct dl_list list;
	u8 nmi_addr[ETH_ALEN];
	struct os_reltime last_seen;
};

/**
 * struct nan_data - Internal data structure for NAN
 * @cfg: Pointer to the NAN configuration structure
 * @nan_started: Flag indicating if NAN has been started
 * @peer_list: List of known peers
 */
struct nan_data {
	struct nan_config *cfg;
	u8 nan_started:1;
	struct dl_list peer_list;
};

struct nan_peer * nan_get_peer(struct nan_data *nan, const u8 *addr);

#endif /* NAN_I_H */
