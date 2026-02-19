/*
 * NAN NDP/NDL state machine testing
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "common/nan_defs.h"
#include "drivers/driver.h"
#include "nan_i.h"
#include "nan_module_tests.h"

#define NAN_TEST_MAX_NAF_LEN     1024
#define NAN_TEST_MAX_PEERS       20
#define NAN_TEST_MAX_ACTIONS     30
#define NAN_TEST_MIN_SLOTS       12
#define NAN_TEST_MAX_LATENCY     3
#define NAN_TEST_PUBLISH_INST_ID 12

static const u8 pub_nmi[] = { 0x00, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
static const u8 pub_ndi[] = { 0x00, 0xAA, 0xAA, 0x00, 0x00, 0x00 };
static const u8 sub_nmi[] = { 0x00, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB };
static const u8 sub_ndi[] = { 0x00, 0xBB, 0xBB, 0xBB, 0x00, 0x00 };

/**
 * struct nan_test_action - NAN test action context
 * @list: Used for global actions list
 * @dev: NAN device for which the action is intended
 * @cb: Callback to be called for the action
 * @ctx: Parameter for the callback function
 *
 * The test utility uses actions to handle asynchronous events between the
 * devices and internally to the local device. When an event is triggered
 * outside of the NAN module context, the event is wrapped by an action item and
 * is processed asynchronously. Examples:
 *
 * NAF transmission: when a NAF is transmitted by a device, it is translated to
 * 2 asynchronous actions: one action to indicate Tx status to the transmitting
 * device and one to indicate an Rx frame to the peer device.
 *
 * NDP event: when an NDP event is fired by the NAN module, it is translated to
 * an asynchronous action to the local device.
 */
struct nan_test_action {
	struct dl_list list;
	struct nan_device *dev;
	int (*cb)(struct nan_device *dev, void *ctx);
	void *ctx;
};

/**
 * nan_test_tx_status_action - NAN test Tx status action context
 * @data: Copy of the original NAF
 * @acked: True iff the NAF was acked
 * @dst: Destination of the NAF
 */
struct nan_test_tx_status_action {
	const struct wpabuf *data;
	bool acked;
	u8 dst[ETH_ALEN];
};

/**
 * nan_test_ndp_notify - NAN test NDP notification handling
 * @type: Notification type
 * @ndp_id: NDP identifier
 * @ssi: Service specific information
 * @ssi_len: Length of service specific information
 */
struct nan_test_ndp_notify {
	enum nan_test_ndp_notify_type type;
	struct nan_ndp_id ndp_id;
	const u8 *ssi;
	size_t ssi_len;
};

/**
 * nan_test_global - Global context for the NAN testing
 * @devs: List of devices. See &struct nan_device.
 * @actions: tracks the NAN actions
 * @elems: Default HT/VHT/HE capabilities elements
 */
struct nan_test_global {
	struct dl_list devs;
	struct dl_list actions;
	struct wpabuf *elems;
};

#define DEV_NOT_INIT_ERR(_dev)                                        \
do {                                                                  \
	if (!(_dev) || !(_dev)->nan)  {                               \
		wpa_printf(MSG_ERROR,                                 \
			   "NAN: %s: device not initialized",         \
			   __func__);                                 \
		return -1;                                            \
	}                                                             \
} while (0)

#define DEV_NOT_INIT_ERR_VOID(_dev)                                   \
do {                                                                  \
	if (!(_dev) || !(_dev)->nan)  {                               \
		wpa_printf(MSG_ERROR,                                 \
			   "NAN: %s: device not initialized",         \
			   __func__);                                 \
		return;                                               \
	}                                                             \
} while (0)


/**
 * nan_test_global_init - Initialize NAN test global data structures
 * @global: NAN test global data structure
 */
static void nan_test_global_init(struct nan_test_global *global)
{
	u8 elems[] = {
		/* HT capabilities */
		0x2d, 0x1a, 0x7e, 0x10, 0x1b, 0xff, 0xff, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,

		/* VHT capabilities */
		0xbf, 0x0c, 0xfa, 0x04, 0x80, 0x03, 0xaa, 0xaa,
		0x00, 0x00, 0xaa, 0xaa, 0x00, 0x00,

		/* HE capabilities */
		0xff, 0x1e, 0x23, 0x01, 0x78, 0xc8, 0x1a, 0x40,
		0x00, 0x1c, 0xbf, 0xce, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xfa, 0xff, 0xfa, 0xff,
		0xfa, 0xff, 0xfa, 0xff, 0xfa, 0xff, 0xfa, 0xff,
	};

	wpa_printf(MSG_INFO, "%s: Enter", __func__);
	os_memset(global, 0, sizeof(struct nan_test_global));
	dl_list_init(&global->devs);
	dl_list_init(&global->actions);

	global->elems = wpabuf_alloc_copy(elems, sizeof(elems));

	wpa_printf(MSG_INFO, "%s: Done\n", __func__);
}


/**
 * nan_test_dev_deinit - De-initialize NAN device
 * @dev: NAN device
 */
static void nan_test_dev_deinit(struct nan_device *dev)
{
	DEV_NOT_INIT_ERR_VOID(dev);

	os_free(dev->pot_avail);
	nan_stop(dev->nan);
	nan_deinit(dev->nan);
	os_memset(dev, 0, sizeof(struct nan_device));
}


/**
 * nan_test_global_deinit - De-initialize NAN test global data structures
 * @global: NAN test global data structure
 */
static void nan_test_global_deinit(struct nan_test_global *global)
{
	struct nan_device *dev, *next;
	struct nan_test_action *action, *action_next;

	wpa_printf(MSG_INFO, "%s: Enter", __func__);

	dl_list_for_each_safe(dev, next, &global->devs, struct nan_device,
			      list) {
		dl_list_del(&dev->list);
		nan_test_dev_deinit(dev);
		os_free(dev);
	}

	dl_list_for_each_safe(action, action_next, &global->actions,
			      struct nan_test_action, list) {
		dl_list_del(&dev->list);
		os_free(action->ctx);
		os_free(action);
	}

	wpabuf_free(global->elems);
}


/**
 * nan_test_add_action - Add a NAN test action to the list of actions
 * @global: NAN test global data structure
 * @dev: NAN device for which the action is intended
 * @cb: Callback to be called for the action
 * @ctx: Parameter for the callback function
 * Returns: A pointer to the newly added action, or NULL on failure
 */
static struct nan_test_action *
nan_test_add_action(struct nan_test_global *global,
		    struct nan_device *dev,
		    int (*cb)(struct nan_device *dev, void *ctx),
		    void *ctx)
{
	struct nan_test_action *action;

	action = os_malloc(sizeof(struct nan_test_action));
	if (!action) {
		wpa_printf(MSG_ERROR, "NAN Test: Failed to allocate action");
		return NULL;
	}

	action->dev = dev;
	action->cb = cb;
	action->ctx = ctx;

	dl_list_add_tail(&global->actions, &action->list);
	return action;
}


/**
 * nan_test_run_actions - Iterate over all NAN test actions and execute them
 * @global: NAN test global data structure
 * Returns: 0 in success, -1 on error
 *
 * Runs as longs as there are actions to run. Exists where there are no more
 * action to perform or on error.
 */
static int nan_test_run_actions(struct nan_test_global *global)
{
	u32 n_actions = 0;

	wpa_printf(MSG_INFO, "%s: Running actions", __func__);

	while (!dl_list_empty(&global->actions)) {
		struct nan_test_action *action =
			dl_list_first(&global->actions, struct nan_test_action,
				      list);
		int ret;

		dl_list_del(&action->list);

		if (++n_actions > NAN_TEST_MAX_ACTIONS) {
			wpa_printf(MSG_ERROR,
				   "NAN Test action: Too many actions executed");
			return -1;
		}

		if (!action->cb || !action->dev) {
			wpa_printf(MSG_ERROR,
				   "NAN Test action: Invalid action");
			return -1;
		}

		wpa_printf(MSG_INFO, "%s: ===> NAN Test action <===",
			   action->dev->name);
		ret = action->cb(action->dev, action->ctx);

		/*
		 * The action context should be freed by the callback function
		 * that understands the content of the context and can properly
		 * handle it.
		 */
		os_free(action);

		if (ret) {
			wpa_printf(MSG_ERROR, "NAN Test action: FAILED");
			return ret;
		}
	}

	wpa_printf(MSG_INFO, "%s: Running actions done", __func__);
	return 0;
}


static int nan_test_start_cb(void *ctx, const struct nan_cluster_config *config)
{
	struct nan_device *dev = ctx;

	DEV_NOT_INIT_ERR(dev);

	return 0;
}


/*
 * nan_test_stop_cb - Stop the NAN device.
 * @ctx: Pointer to the &struct nan_device
 */
static void nan_test_stop_cb(void *ctx)
{
	struct nan_device *dev = ctx;

	DEV_NOT_INIT_ERR_VOID(dev);

	wpa_printf(MSG_INFO, "%s: %s: Done", __func__, dev->name);
}


static int nan_test_nan_ndp_action(struct nan_device *dev, void *ctx)
{
	struct nan_ndp_params *params = ctx;
	int ret;

	wpa_printf(MSG_INFO, "%s: %s: type=%u, peer_nmi=" MACSTR,
		   dev->name, __func__, params->type,
		   MAC2STR(params->ndp_id.peer_nmi));

	ret = nan_handle_ndp_setup(dev->nan, params);

	os_free(params);

	return ret;
}


/*
 * nan_ndp_notify_action - Default NAN NDP notification handling
 * @dev: NAN device
 * @ctx: Pointer to &struct nan_test_ndp_notify
 */
static int nan_ndp_notify_action(struct nan_device *dev, void *ctx)
{
	struct nan_test_ndp_notify *notify = ctx;
	struct nan_test_action *action;
	int ret;

	wpa_printf(MSG_INFO, "%s: %s: type=%u, peer_nmi=" MACSTR,
		   dev->name, __func__, notify->type,
		   MAC2STR(notify->ndp_id.peer_nmi));

	ret = -1;
	if (notify->type == NAN_TEST_NDP_NOTIFY_REQUEST) {
		struct nan_ndp_params *params;
		struct nan_device *curd;
		bool found = false;

		dl_list_for_each(curd, &dev->global->devs, struct nan_device,
				 list) {
			if (ether_addr_equal(curd->nmi,
					     notify->ndp_id.peer_nmi)) {
				found = true;
				break;
			}
		}

		if (!found) {
			wpa_printf(MSG_ERROR, "Peer device not found");
			ret = -1;
			goto out;
		}

		params = os_zalloc(sizeof(struct nan_ndp_params));
		if (!params) {
			ret = -1;
			goto out;
		}

		params->type = NAN_NDP_ACTION_RESP;
		os_memcpy(params->ndp_id.peer_nmi, notify->ndp_id.peer_nmi,
			  ETH_ALEN);
		os_memcpy(params->ndp_id.init_ndi, notify->ndp_id.init_ndi,
			  ETH_ALEN);
		params->ndp_id.id = notify->ndp_id.id;
		params->qos.min_slots = NAN_TEST_MIN_SLOTS;
		params->qos.max_latency = NAN_TEST_MAX_LATENCY;

		if (dev->conf->ndp_confs[dev->n_ndps].accept_request) {
			wpa_printf(MSG_INFO, "%s: Accepting request",
				   dev->name);

			os_memcpy(params->u.resp.resp_ndi, pub_ndi, ETH_ALEN);
			params->u.resp.status = NAN_NDP_STATUS_ACCEPTED;
			dev->conf->schedule_cb(&params->sched);
			params->sched.elems = dev->global->elems;
			params->sched_valid = 1;
		} else {
			wpa_printf(MSG_INFO, "%s: Rejecting request",
				   dev->name);

			params->u.resp.status = NAN_NDP_STATUS_REJECTED;
			params->u.resp.reason_code =
				dev->conf->ndp_confs[dev->n_ndps].reason;
		}

		action = nan_test_add_action(dev->global, dev,
					     nan_test_nan_ndp_action,
					     params);
		if (action)
			ret = 0;
	} else if (notify->type == NAN_TEST_NDP_NOTIFY_RESPONSE) {
		struct nan_ndp_params *params;

		params = os_zalloc(sizeof(struct nan_ndp_params));
		if (!params) {
			ret = -1;
			goto out;
		}

		params->type = NAN_NDP_ACTION_CONF;
		os_memcpy(params->ndp_id.peer_nmi, notify->ndp_id.peer_nmi,
			  ETH_ALEN);
		os_memcpy(params->ndp_id.init_ndi, notify->ndp_id.init_ndi,
			  ETH_ALEN);
		params->ndp_id.id = notify->ndp_id.id;

		params->qos.min_slots = NAN_TEST_MIN_SLOTS;
		params->qos.max_latency = NAN_TEST_MAX_LATENCY;

		if (dev->conf->ndp_confs[dev->n_ndps].accept_request) {
			wpa_printf(MSG_INFO, "%s: Accepting response",
				   dev->name);

			os_memcpy(params->u.resp.resp_ndi, sub_ndi, ETH_ALEN);
			params->u.resp.status = NAN_NDP_STATUS_ACCEPTED;

			if (!dev->conf->schedule_conf_cb) {
				wpa_printf(MSG_ERROR,
					   "%s: No schedule conf cb defined",
					   dev->name);
				os_free(params);
				ret = -1;
				goto out;
			}

			dev->conf->schedule_conf_cb(&params->sched);
			params->sched.elems = dev->global->elems;
			params->sched_valid = 1;

		} else {
			wpa_printf(MSG_INFO, "%s: Rejecting response",
				   dev->name);

			params->u.resp.status = NAN_NDP_STATUS_REJECTED;
			params->u.resp.reason_code =
				dev->conf->ndp_confs[dev->n_ndps].reason;
		}

		action = nan_test_add_action(dev->global, dev,
					     nan_test_nan_ndp_action,
					     params);
		if (action)
			ret = 0;
	} else if (notify->type ==
		   dev->conf->ndp_confs[dev->n_ndps].expected_result) {
		ret = 0;
		if (notify->type == NAN_TEST_NDP_NOTIFY_CONNECTED) {
			if (dev->conf->ndp_confs[dev->n_ndps].
			    term_once_connected) {
				struct nan_ndp_params *params;

				wpa_printf(MSG_INFO,
					   "%s: Connected successfully. Now term",
					   dev->name);

				params = os_zalloc(sizeof(*params));
				if (!params) {
					ret = -1;
					goto out;
				}

				params->type = NAN_NDP_ACTION_TERM;
				os_memcpy(params->ndp_id.peer_nmi,
					  notify->ndp_id.peer_nmi,
					  ETH_ALEN);
				os_memcpy(params->ndp_id.init_ndi,
					  notify->ndp_id.init_ndi,
					  ETH_ALEN);
				params->ndp_id.id = notify->ndp_id.id;

				action = nan_test_add_action(
					dev->global, dev,
					nan_test_nan_ndp_action, params);
				if (action)
					ret = 0;
			}
		}
	} else if (notify->type == NAN_TEST_NDP_NOTIFY_DISCONNECTED &&
		   dev->connected_notify_received) {
		wpa_printf(MSG_INFO,
			   "%s: Disconnected after connected as expected. Test case done",
			   dev->name);
		ret = 0;
	} else {
		wpa_printf(MSG_ERROR,
			   "%s: Unexpected notify: type=%u expected=%u",
			   dev->name, notify->type,
			   dev->conf->ndp_confs[dev->n_ndps].expected_result);
		ret = -1;
	}

out:
	os_free((void *) notify->ssi);
	os_free(notify);
	return ret;
}


static void nan_test_ndp_action(struct nan_device *dev, enum
				nan_test_ndp_notify_type type,
				struct nan_ndp_id *ndp_id,
				u8 publish_inst_id,
				const u8 *ssi, size_t ssi_len,
				enum nan_cipher_suite_id csid,
				const u8 *pmkid)
{
	struct nan_test_ndp_notify *notify;

	DEV_NOT_INIT_ERR_VOID(dev);

	wpa_printf(MSG_INFO, "%s: %s: Enter: type=%u",
		   dev->name, __func__, type);

	notify = os_zalloc(sizeof(struct nan_test_ndp_notify));
	if (!notify) {
		wpa_printf(MSG_ERROR,
			   "Failed allocation: nan_test_ndp_notify");
		return;
	}

	notify->type = type;
	os_memcpy(&notify->ndp_id, ndp_id, sizeof(notify->ndp_id));

	if (ssi && ssi_len) {
		notify->ssi = os_memdup(ssi, ssi_len);
		if (!notify->ssi) {
			wpa_printf(MSG_ERROR, "Failed allocation: ssi");
			os_free(notify);
			return;
		}
		notify->ssi_len = ssi_len;
	}

	if (nan_test_add_action(dev->global, dev, nan_ndp_notify_action,
				notify))
		return;

	os_free((void *) notify->ssi);
	os_free(notify);
}


/**
 * nan_test_ndp_action_notfi_cb - Callback for NDP request
 * @ctx: Pointer to &struct nan_device
 * @params: NDP action notification parameters
 *
 * The handling of the event is done asynchronously through the NAN test actions
 * processing.
 */
static void
nan_test_ndp_action_notfi_cb(void *ctx,
			     struct nan_ndp_action_notif_params *params)
{
	struct nan_device *dev = ctx;
	enum nan_test_ndp_notify_type type;

	DEV_NOT_INIT_ERR_VOID(dev);

	if (params->is_request)
		type = NAN_TEST_NDP_NOTIFY_REQUEST;
	else
		type = NAN_TEST_NDP_NOTIFY_RESPONSE;

	nan_test_ndp_action(dev, type, &params->ndp_id,
			    params->publish_inst_id, params->ssi,
			    params->ssi_len, params->csid, params->pmkid);
}


/**
 * nan_test_ndp_connected_cb - Callback for NDP connected
 * @ctx: Pointer to &struct nan_device
 * @params: NDP action notification parameters
 *
 * The handling of the event is done asynchronously through the NAN test actions
 * processing.
 */
static void nan_test_ndp_connected_cb(void *ctx,
				      struct nan_ndp_connection_params *params)
{
	struct nan_device *dev = ctx;
	struct nan_peer_schedule sched;
	struct nan_peer_potential_avail pot;

	DEV_NOT_INIT_ERR_VOID(dev);

	wpa_printf(MSG_INFO,
		   "%s: %s: Enter. local_ndi=" MACSTR " peer_ndi=" MACSTR,
		   dev->name, __func__,
		   MAC2STR(params->local_ndi), MAC2STR(params->peer_ndi));

	nan_peer_get_schedule_info(dev->nan, params->ndp_id.peer_nmi, &sched);
	nan_peer_get_pot_avail(dev->nan, params->ndp_id.peer_nmi, &pot);

	nan_test_ndp_action(dev, NAN_TEST_NDP_NOTIFY_CONNECTED, &params->ndp_id,
			    0, params->ssi, params->ssi_len, NAN_CS_NONE, NULL);

	dev->connected_notify_received = true;
}


/**
 * nan_test_ndp_disconnected_cb - Callback for NDP disconnected
 * @ctx: Pointer to &struct nan_device
 * @ndp_id: NDP identifier
 * @local_ndi: Local NDI address
 * @peer_ndi: Peer NDI address
 * @reason: Reason for disconnection
 *
 * The handling of the event is done asynchronously through the NAN test actions
 * processing.
 */
static void nan_test_ndp_disconnected_cb(void *ctx, struct nan_ndp_id *ndp_id,
					 const u8 *local_ndi,
					 const u8 *peer_ndi,
					 enum nan_reason reason)
{
	struct nan_device *dev = ctx;

	DEV_NOT_INIT_ERR_VOID(dev);

	wpa_printf(MSG_INFO, "%s: %s: Enter", dev->name, __func__);

	nan_test_ndp_action(dev, NAN_TEST_NDP_NOTIFY_DISCONNECTED,
			    ndp_id, 0, NULL, 0, NAN_CS_NONE, NULL);

	dev->disconnected_notify_received = true;
}


/**
 * nan_test_send_naf_cb_action - NAN test action to send a NAN to a device
 * @dev: NAN device
 * @ctx: Pointer to a buffer holding the NAF to be sent
 */
static int nan_test_send_naf_cb_action(struct nan_device *dev, void *ctx)
{
	struct wpabuf *data = ctx;
	int ret;

	DEV_NOT_INIT_ERR(dev);

	wpa_printf(MSG_INFO, "%s: %s: Enter", dev->name, __func__);
	wpa_hexdump(MSG_DEBUG, "NAN Test: NAF:", wpabuf_head(data),
		    wpabuf_len(data));

	ret = nan_action_rx(dev->nan, wpabuf_head(data), wpabuf_len(data));
	wpabuf_free(data);
	return ret;
}


/**
 * nan_test_tx_status_action - NAN test action to send Tx status to a device
 * @dev: NAN device
 * @ctx: Pointer to &struct nan_test_tx_status_action
 */
static int nan_test_tx_status_action(struct nan_device *dev, void *ctx)
{
	struct nan_test_tx_status_action *tx_status = ctx;
	int ret;

	DEV_NOT_INIT_ERR(dev);

	wpa_printf(MSG_INFO, "%s: %s: enter", dev->name, __func__);

	ret = nan_tx_status(dev->nan, tx_status->dst,
			    wpabuf_head(tx_status->data),
			    wpabuf_len(tx_status->data), tx_status->acked);

	wpabuf_free((struct wpabuf *)tx_status->data);
	os_free(tx_status);
	return ret;
}


/**
 * nan_test_send_naf_cb - NAN send NAF callback function
 * @ctx: Pointer to &struct nan_device
 * @dst: Destination NAN Management Interface address
 * @src: Source NAN Management Interface address
 * @cluster_id: NAN Cluster ID
 *
 * The callback builds the management frame and creates the following NAN test
 * actions:
 * - NAN test tx status action: to be sent to the transmitting device
 * - NAN test send NAF action: to be sent to the destination device.
 */
static int nan_test_send_naf_cb(void *ctx, const u8 *dst, const u8 *src,
				const u8 *cluster_id, struct wpabuf *buf)
{
	struct nan_device *dev = ctx;
	struct nan_device *curd;
	struct ieee80211_hdr *hdr;
	struct wpabuf *data = NULL;
	struct nan_test_tx_status_action *tx_status = NULL;
	struct nan_test_action *dev_action, *cur_action;
	bool found = false;

	DEV_NOT_INIT_ERR(dev);

	if (!dst)
		return -1;

	wpa_printf(MSG_INFO, "%s: %s: Enter " MACSTR, __func__, dev->name,
		   MAC2STR(dst));

	dl_list_for_each(curd, &dev->global->devs, struct nan_device, list) {
		if (ether_addr_equal(curd->nmi, dst)) {
			found = true;
			break;
		}
	}

	if (!found) {
		wpa_printf(MSG_ERROR, "%s: Destination device not found",
			   __func__);
		return -1;
	}

	/* Prepare action to send the frame to the peer */
	data = wpabuf_alloc(sizeof(struct ieee80211_hdr) + wpabuf_len(buf));
	if (!data) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate NAF", __func__);
		return -1;
	}

	hdr = wpabuf_put(data, sizeof(struct ieee80211_hdr));
	hdr->frame_control =
		IEEE80211_FC(WLAN_FC_TYPE_MGMT, WLAN_FC_STYPE_ACTION);

	os_memcpy(hdr->addr1, dst, ETH_ALEN);
	if (!src)
		os_memcpy(hdr->addr2, dev->nmi, ETH_ALEN);
	else
		os_memcpy(hdr->addr2, src, ETH_ALEN);
	if (cluster_id)
		os_memcpy(hdr->addr3, cluster_id, ETH_ALEN);

	wpabuf_put_data(data, wpabuf_head(buf), wpabuf_len(buf));

	/* Prepare action to send Tx status */
	tx_status = os_malloc(sizeof(struct nan_test_tx_status_action));
	if (!tx_status) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate Tx status",
			   __func__);
		goto fail;
	}

	tx_status->data = wpabuf_dup(data);
	if (!tx_status->data) {
		wpa_printf(MSG_ERROR, "%s: Failed to allocate Tx status data",
			   __func__);
		goto fail;
	}

	os_memcpy(tx_status->dst, dst, ETH_ALEN);
	tx_status->acked = 1;

	/* First send the TX status */
	dev_action = nan_test_add_action(dev->global, dev,
					 nan_test_tx_status_action, tx_status);
	if (!dev_action)
		goto fail;

	/* And then deliver the frame */
	cur_action = nan_test_add_action(curd->global, curd,
					 nan_test_send_naf_cb_action, data);
	if (!cur_action)
		goto fail;

	return 0;

fail:
	wpabuf_free(data);
	if (tx_status)
		wpabuf_free((struct wpabuf *) tx_status->data);

	os_free(tx_status);

	return -1;
}


/**
 * nan_test_get_chans_cb - Get NAN supported channels callback
 * @ctx: Pointer to &struct nan_device
 * @map_id: Channel map identifier
 * @chans: Pointer to &struct nan_channels to be filled with supported channels
 */
static int nan_test_get_chans_cb(void *ctx, u8 map_id,
				 struct nan_channels *chans)
{
	struct nan_device *dev = ctx;

	DEV_NOT_INIT_ERR(dev);

	wpa_printf(MSG_INFO, "%s: %s: Enter", dev->name, __func__);

	return dev->conf->get_chans_cb(chans);
}


/**
 * nan_test_is_valid_publish_id_cb - Check if the publish instance ID is valid
 * @ctx: Pointer to &struct nan_device
 * @instance_id: Publish instance ID
 * @service_id: Buffer to be filled with the service ID
 * Returns true if the instance ID is valid, false otherwise
 */
static bool nan_test_is_valid_publish_id_cb(void *ctx, u8 instance_id,
					    u8 *service_id)
{
	if (instance_id != NAN_TEST_PUBLISH_INST_ID)
		return false;

	os_memset(service_id, 0xaa, NAN_SERVICE_ID_LEN);
	return true;
}


/**
 * nan_test_dev_init - Initialize a test device instance
 * @dev: the instance of the device to initialize
 */
static int nan_test_dev_init(struct nan_device *dev)
{
	struct nan_config nan;

	os_memset(&nan, 0, sizeof(nan));
	nan.cb_ctx = dev;

	nan.start = nan_test_start_cb;
	nan.stop = nan_test_stop_cb;
	nan.ndp_action_notif = nan_test_ndp_action_notfi_cb;
	nan.ndp_connected = nan_test_ndp_connected_cb;
	nan.ndp_disconnected = nan_test_ndp_disconnected_cb;
	nan.send_naf = nan_test_send_naf_cb;
	nan.get_chans = nan_test_get_chans_cb;
	nan.is_valid_publish_id = nan_test_is_valid_publish_id_cb;

	/* Awake on every DW on 2 GHz and 5 GHz */
	nan.dev_capa.cdw_info = 0x9;
	nan.dev_capa.supported_bands = NAN_DEV_CAPA_SBAND_2G |
		NAN_DEV_CAPA_SBAND_5G |
		NAN_DEV_CAPA_SBAND_6G;

	nan.dev_capa.op_mode = NAN_DEV_CAPA_OP_MODE_PHY_MODE;
	nan.dev_capa.n_antennas = 0x22;
	nan.dev_capa.channel_switch_time = 10;
	nan.dev_capa.capa = 0;

	nan.dev_capa_ext_reg_info = 0;
	nan.dev_capa_ext_pairing_npk_caching =
		NAN_DEV_CAPA_EXT_INFO_1_PAIRING_SETUP |
		NAN_DEV_CAPA_EXT_INFO_1_NPK_NIK_CACHING;

	dev->nan = nan_init(&nan);
	if (!dev->nan) {
		wpa_printf(MSG_DEBUG, "NAN: Failed to init");
		return -1;
	}

	return 0;
}


/**
 * nan_test_start_dev - Start a NAN test device
 * @global: NAN test global data structure
 * @name: Name of the device
 * @nmi: NAN Management interface address
 * @cconf: NAN cluster configuration
 * @dconf: Test device configuration
 */
static struct nan_device *
nan_test_start_dev(struct nan_test_global *global,
		   const char *name, const u8 *nmi,
		   struct nan_cluster_config *conf,
		   const struct nan_test_dev_conf *dconf)
{
	struct nan_device *dev;
	int ret;
	size_t nlen;

	dev = os_zalloc(sizeof(struct nan_device));
	if (!dev)
		return NULL;

	nlen = os_strlen(name);
	if (nlen >= sizeof(dev->name))
		nlen = sizeof(dev->name) - 1;
	os_memcpy(dev->name, name, nlen);
	os_memcpy(dev->nmi, nmi, sizeof(dev->nmi));
	dl_list_init(&dev->list);
	dev->global = global;

	if (dconf->pot_avail_len) {
		dev->pot_avail = os_memdup(dconf->pot_avail,
					   dconf->pot_avail_len);
		if (!dev->pot_avail)
			goto fail;
		dev->pot_avail_len = dconf->pot_avail_len;
	}

	ret = nan_test_dev_init(dev);
	if (ret)
		goto fail;

	dev->conf = dconf;
	ret = nan_start(dev->nan, conf);
	if (ret)
		goto fail;

	dl_list_add(&global->devs, &dev->list);
	return dev;

fail:
	nan_test_dev_deinit(dev);
	os_free(dev);
	return NULL;
}


/**
 * nan_test_setup_devices - Setup the test devices
 * @global: NAN test global data structure
 * @pub_conf: Publisher test configuration
 * @sub_conf: Subscriber test configuration
 */
static struct nan_device *
nan_test_setup_devices(struct nan_test_global *global,
		       const struct nan_test_dev_conf *pub_conf,
		       const struct nan_test_dev_conf *sub_conf)
{
	struct nan_cluster_config cconf = {
		.master_pref = 2,
		.dual_band = 1,
	};
	const u8 pot_avail[] = {
		0x12, 0x0c, 0x00, 0x01, 0x20, 0x00, 0x07, 0x00,
		0x1a, 0x00, 0x11, 0x51, 0xff, 0x07, 0x00,
	};

	struct nan_device *pub, *sub;

	wpa_printf(MSG_INFO, "%s: Enter\n", __func__);

	pub = nan_test_start_dev(global, "publisher", pub_nmi, &cconf,
				 pub_conf);
	if (!pub)
		goto fail;

	sub = nan_test_start_dev(global, "subscriber", sub_nmi, &cconf,
				 sub_conf);
	if (!sub)
		goto fail;

	nan_add_peer(pub->nan, sub_nmi, pot_avail, sizeof(pot_avail));
	nan_add_peer(sub->nan, pub_nmi, pot_avail, sizeof(pot_avail));

	wpa_printf(MSG_INFO, "\n%s: Done\n", __func__);
	return sub;

fail:
	wpa_printf(MSG_INFO, "\n%s: Fail\n", __func__);
	return NULL;
}


static int nan_test_ndp_request(struct nan_device *sub)
{
	struct nan_ndp_params *params;
	struct nan_test_action *action;

	DEV_NOT_INIT_ERR(sub);

	params = os_zalloc(sizeof(struct nan_ndp_params));
	if (!params) {
		wpa_printf(MSG_ERROR, "Failed allocation: nan_ndp_params");
		return -1;
	}

	params->type = NAN_NDP_ACTION_REQ;
	os_memcpy(params->ndp_id.peer_nmi, pub_nmi, ETH_ALEN);
	os_memcpy(params->ndp_id.init_ndi, sub_ndi, ETH_ALEN);
	params->ndp_id.id = ++sub->counter;
	params->qos.min_slots = NAN_TEST_MIN_SLOTS;
	params->qos.max_latency = NAN_TEST_MAX_LATENCY;

	/* Use the device specific schedule callback */
	sub->conf->schedule_cb(&params->sched);
	params->sched_valid = 1;
	params->sched.elems = sub->global->elems;

	params->u.req.publish_inst_id = NAN_TEST_PUBLISH_INST_ID;
	os_memset(params->u.req.service_id, 0xaa, NAN_SERVICE_ID_LEN);

	action = nan_test_add_action(sub->global, sub, nan_test_nan_ndp_action,
				     params);
	if (action)
		return 0;

	wpa_printf(MSG_ERROR, "Failed adding NDP request action");
	os_free(params);

	return -1;
}


/**
 * nan_test_ndp_setup - test NDP setup
 * @global: NAN test global data structure
 * @pub_conf: Publisher test configuration
 * @sub_conf: Subscriber test configuration
 *
 * Create the test devices, perform the basic publish/subscribe/match to allow
 * NDP establishment and trigger an NDP request flow based on the actions
 * mechanism.
 */
static struct nan_device *
nan_test_ndp_setup(struct nan_test_global *global,
		   const struct nan_test_dev_conf *pub_conf,
		   const struct nan_test_dev_conf *sub_conf)
{
	if (pub_conf->n_ndps != sub_conf->n_ndps || !pub_conf->n_ndps ||
	    pub_conf->n_ndps >= NAN_MAX_NUM_NDPS) {
		wpa_printf(MSG_DEBUG,
			   "NAN Test: Publisher and Subscriber n_ndps mismatch or invalid");
		return NULL;
	}

	return nan_test_setup_devices(global, pub_conf, sub_conf);
}


static int nan_test_verify_expected_result(struct nan_device *dev)
{
	DEV_NOT_INIT_ERR(dev);

	if (dev->conf->ndp_confs[dev->n_ndps].expected_result ==
	    NAN_TEST_NDP_NOTIFY_CONNECTED &&
	    !dev->connected_notify_received) {
		wpa_printf(MSG_ERROR,
			   "%s: Expected connected notify not received",
			   dev->name);
		return -1;
	}

	if (dev->conf->ndp_confs[dev->n_ndps].expected_result ==
	    NAN_TEST_NDP_NOTIFY_DISCONNECTED &&
	    !dev->disconnected_notify_received) {
		wpa_printf(MSG_ERROR,
			   "%s: Expected disconnected notify not received",
			   dev->name);
		return -1;
	}

	return 0;
}


static int nan_test_iteration_done(struct nan_test_global *global)
{
	struct nan_device *dev;
	int ret;

	dl_list_for_each(dev, &global->devs, struct nan_device, list) {
		ret = nan_test_verify_expected_result(dev);

		if (ret)
			return ret;

		dev->connected_notify_received = false;
		dev->disconnected_notify_received = false;
		dev->n_ndps++;
	}

	return 0;
}


/**
 * nan_test_run - Run the NAN tests
 * Iterates over all tests cases and for each test case initializes the global
 * context, creates the NAN devices and perform the test case.
 */
static int nan_test_run(void)
{
	struct nan_test_global global;
	const struct nan_test_case *curr_tc;
	bool all_failed = false;

	while ((curr_tc = nan_test_case_get_next())) {
		struct nan_device *sub;
		bool failed = false;
		size_t i;

		wpa_printf(MSG_INFO,
			   "\n======> NAN TEST CASE: %s <======\n",
			   curr_tc->name);

		nan_test_global_init(&global);
		sub = nan_test_ndp_setup(&global,
					 &curr_tc->pub_conf,
					 &curr_tc->sub_conf);
		if (!sub) {
			wpa_printf(MSG_ERROR,
				   "NAN Test: Failed to setup devices");
			nan_test_global_deinit(&global);
			failed = true;
			continue;
		}

		for (i = 0; i < sub->conf->n_ndps; i++) {
			int ret = nan_test_ndp_request(sub);

			if (!ret)
				ret = nan_test_run_actions(&global);

			if (!ret)
				ret = nan_test_iteration_done(&global);

			wpa_printf(MSG_INFO,
				   "\n======> NAN TEST CASE: %s: iter=%zu: result=%s <======\n",
				   curr_tc->name, i,
				   ret ? "FAILED" : "SUCCESS");

			if (ret) {
				failed = true;
				break;
			}
		}

		wpa_printf(MSG_INFO,
			   "\n======> NAN TEST CASE: %s: Done. Result=%s <======\n",
			   curr_tc->name, failed ? "FAILED" : "SUCCESS");

		all_failed |= failed;

		nan_test_global_deinit(&global);
	}

	return all_failed ? -1 : 0;
}


int nan_module_tests(void)
{
	return nan_test_run();
}
