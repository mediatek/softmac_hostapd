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
#include "common/ieee802_11_defs.h"
#include "common/nan_defs.h"
#include "nan.h"

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

struct nan_attrs_entry {
	struct dl_list list;
	const u8 *ptr;
	u16 len;
};

struct nan_attrs {
	struct dl_list serv_desc_ext;
	struct dl_list avail;
	struct dl_list ndc;
	struct dl_list dev_capa;
	struct dl_list element_container;

	const u8 *ndp;
	const u8 *ndl;
	const u8 *ndl_qos;
	const u8 *cipher_suite_info;
	const u8 *sec_ctxt_info;
	const u8 *shared_key_desc;

	u16 ndp_len;
	u16 ndl_len;
	u16 ndl_qos_len;
	u16 cipher_suite_info_len;
	u16 sec_ctxt_info_len;
	u16 shared_key_desc_len;
};

struct nan_msg {
	u8 oui_type;
	u8 oui_subtype;
	struct nan_attrs attrs;

	/* The full frame is required for the NDP security flows, that compute
	 * the NDP authentication token over the entire frame body. */
	const struct ieee80211_mgmt *mgmt;
	size_t len;
};

struct nan_peer * nan_get_peer(struct nan_data *nan, const u8 *addr);
bool nan_is_naf(const struct ieee80211_mgmt *mgmt, size_t len);
int nan_parse_attrs(struct nan_data *nan, const u8 *data, size_t len,
		    struct nan_attrs *attrs);
int nan_parse_naf(struct nan_data *nan, const struct ieee80211_mgmt *mgmt,
		  size_t len, struct nan_msg *msg);
void nan_attrs_clear(struct nan_data *nan, struct nan_attrs *attrs);

#endif /* NAN_I_H */
