/*
 * Wi-Fi Aware - NAN module utils
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include "common.h"
#include "common/wpa_common.h"
#include "nan_i.h"


static void nan_attrs_clear_list(struct nan_data *nan,
				 struct dl_list *list)
{
	struct nan_attrs_entry *entry, *pentry;

	dl_list_for_each_safe(entry, pentry, list, struct nan_attrs_entry,
			      list) {
		dl_list_del(&entry->list);
		os_free(entry);
	}
}


/*
 * nan_attrs_clear - Free data from NAN parsing
 * @nan: NAN module context from nan_init()
 * @attrs: Parsed nan_attrs
 */
void nan_attrs_clear(struct nan_data *nan, struct nan_attrs *attrs)
{
	nan_attrs_clear_list(nan, &attrs->serv_desc_ext);
	nan_attrs_clear_list(nan, &attrs->avail);
	nan_attrs_clear_list(nan, &attrs->ndc);
	nan_attrs_clear_list(nan, &attrs->dev_capa);
	nan_attrs_clear_list(nan, &attrs->element_container);

	os_memset(attrs, 0, sizeof(*attrs));
}


/*
 * nan_parse_attrs - Parse NAN attributes
 * @nan: NAN module context from nan_init()
 * @data: Buffer holding the attributes
 * @len: Length of &data
 * @attrs: On return would hold the parsed attributes
 * Returns: 0 on success; positive or negative indicate an error
 *
 * Note: In case of success, the caller must free temporary memory allocations
 * by calling nan_attrs_clear() when the parsed data is not needed anymore.
 */
int nan_parse_attrs(struct nan_data *nan, const u8 *data, size_t len,
		    struct nan_attrs *attrs)
{
	struct nan_attrs_entry *entry;
	const u8 *pos = data;
	const u8 *end = pos + len;

	os_memset(attrs, 0, sizeof(*attrs));

	dl_list_init(&attrs->serv_desc_ext);
	dl_list_init(&attrs->avail);
	dl_list_init(&attrs->ndc);
	dl_list_init(&attrs->dev_capa);
	dl_list_init(&attrs->element_container);

	while (end - pos > 3) {
		u8 id = *pos++;
		u16 attr_len = WPA_GET_LE16(pos);

		pos += 2;
		if (attr_len > end - pos)
			goto fail;

		switch (id) {
		case NAN_ATTR_SDEA:
			entry = os_zalloc(sizeof(*entry));
			if (!entry)
				goto fail;

			entry->ptr = pos;
			entry->len = attr_len;
			dl_list_add_tail(&attrs->serv_desc_ext, &entry->list);
			break;
		case NAN_ATTR_DEVICE_CAPABILITY:
			/* Validate Device Capability attribute length */
			if (attr_len < sizeof(struct nan_device_capa))
				break;

			entry = os_zalloc(sizeof(*entry));
			if (!entry)
				goto fail;

			entry->ptr = pos;
			entry->len = attr_len;
			dl_list_add_tail(&attrs->dev_capa, &entry->list);
			break;
		case NAN_ATTR_NDP:
			/* Validate minimal NDP attribute length */
			if (attr_len < sizeof(struct ieee80211_ndp))
				break;

			attrs->ndp = pos;
			attrs->ndp_len = attr_len;
			break;
		case NAN_ATTR_NAN_AVAILABILITY:
			/* Validate minimal Availability attribute length */
			if (attr_len < sizeof(struct nan_avail))
				break;

			entry = os_zalloc(sizeof(*entry));
			if (!entry)
				goto fail;

			entry->ptr = pos;
			entry->len = attr_len;
			dl_list_add_tail(&attrs->avail, &entry->list);
			break;
		case NAN_ATTR_NDC:
			/* Validate minimal NDC attribute length */
			if (attr_len < sizeof(struct ieee80211_ndc))
				break;

			entry = os_zalloc(sizeof(*entry));
			if (!entry)
				goto fail;

			entry->ptr = pos;
			entry->len = attr_len;
			dl_list_add_tail(&attrs->ndc, &entry->list);
			break;
		case NAN_ATTR_NDL:
			/* Validate minimal NDL attribute length */
			if (attr_len < sizeof(struct ieee80211_ndl))
				break;

			attrs->ndl = pos;
			attrs->ndl_len = attr_len;
			break;
		case NAN_ATTR_NDL_QOS:
			/* Validate QoS attribute length */
			if (attr_len < sizeof(struct ieee80211_nan_qos))
				break;

			attrs->ndl_qos = pos;
			attrs->ndl_qos_len = attr_len;
			break;
		case NAN_ATTR_ELEM_CONTAINER:
			/* Validate minimal Element Container attribute length
			 */
			if (attr_len < 1)
				break;

			entry = os_zalloc(sizeof(*entry));
			if (!entry)
				goto fail;

			entry->ptr = pos;
			entry->len = attr_len;
			dl_list_add_tail(&attrs->element_container,
					 &entry->list);
			break;
		case NAN_ATTR_CSIA:
			if (attr_len < sizeof(struct nan_cipher_suite_info) +
			    sizeof(struct nan_cipher_suite))
				break;

			attrs->cipher_suite_info = pos;
			attrs->cipher_suite_info_len = attr_len;
			break;
		case NAN_ATTR_SCIA:
			if (attr_len < sizeof(struct nan_sec_ctxt))
				break;

			attrs->sec_ctxt_info = pos;
			attrs->sec_ctxt_info_len = attr_len;
			break;
		case NAN_ATTR_SHARED_KEY_DESCR:
			if (attr_len < sizeof(struct nan_shared_key) +
			    sizeof(struct wpa_eapol_key))
				break;

			attrs->shared_key_desc = pos;
			attrs->shared_key_desc_len = attr_len;
			break;
		case NAN_ATTR_MASTER_INDICATION:
		case NAN_ATTR_CLUSTER:
		case NAN_ATTR_NAN_ATTR_SERVICE_ID_LIST:
		case NAN_ATTR_SDA:
		case NAN_ATTR_CONN_CAPA:
		case NAN_ATTR_WLAN_INFRA:
		case NAN_ATTR_P2P_OPER:
		case NAN_ATTR_IBSS:
		case NAN_ATTR_MESH:
		case NAN_ATTR_FURTHER_NAN_SD:
		case NAN_ATTR_FURTHER_AVAIL_MAP:
		case NAN_ATTR_COUNTRY_CODE:
		case NAN_ATTR_RANGING:
		case NAN_ATTR_CLUSTER_DISCOVERY:
		case NAN_ATTR_UNALIGNED_SCHEDULE:
		case NAN_ATTR_RANGING_INFO:
		case NAN_ATTR_RANGING_SETUP:
		case NAN_ATTR_FTM_RANGING_REPORT:
		case NAN_ATTR_EXT_WLAN_INFRA:
		case NAN_ATTR_EXT_P2P_OPER:
		case NAN_ATTR_EXT_IBSS:
		case NAN_ATTR_EXT_MESH:
		case NAN_ATTR_PUBLIC_AVAILABILITY:
		case NAN_ATTR_SUBSC_SERVICE_ID_LIST:
		case NAN_ATTR_NDP_EXT:
		case NAN_ATTR_DCEA:
		case NAN_ATTR_NIRA:
		case NAN_ATTR_BPBA:
		case NAN_ATTR_S3:
		case NAN_ATTR_TPEA:
		case NAN_ATTR_VENDOR_SPECIFIC:
			wpa_printf(MSG_DEBUG, "NAN: ignore attr=%u", id);
			break;
		default:
			wpa_printf(MSG_DEBUG, "NAN: unknown attr=%u", id);
			break;
		}

		pos += attr_len;
	}

	/* Parsing is considered success only if all attributes were consumed */
	if (pos == end)
		return 0;

fail:
	nan_attrs_clear(nan, attrs);
	return -1;
}


/*
 * nan_is_naf - Check if a given frame is a NAN Action frame
 * @mgmt: NAN Action frame
 * @len: Length of the Management frame in octets
 * Returns: true if NAF; otherwise false
 */
bool nan_is_naf(const struct ieee80211_mgmt *mgmt, size_t len)
{
	u8 subtype;

	/*
	 * 802.11 header + category + NAN Action frame minimal + subtype (1)
	 */
	if (len < IEEE80211_MIN_ACTION_LEN(naf)) {
		wpa_printf(MSG_DEBUG, "NAN: Too short NAN frame");
		return false;
	}

	if (mgmt->u.action.u.naf.action != WLAN_PA_VENDOR_SPECIFIC ||
	    WPA_GET_BE24(mgmt->u.action.u.naf.oui) != OUI_WFA ||
	    mgmt->u.action.u.naf.oui_type != NAN_NAF_OUI_TYPE)
		return false;

	subtype = mgmt->u.action.u.naf.subtype;

	if (mgmt->u.action.category != WLAN_ACTION_PUBLIC &&
	    !(subtype >= NAN_SUBTYPE_DATA_PATH_REQUEST &&
	      subtype <= NAN_SUBTYPE_DATA_PATH_TERMINATION &&
	      mgmt->u.action.category == WLAN_ACTION_PROTECTED_DUAL)) {
		wpa_printf(MSG_DEBUG, "NAN: Invalid action category for NAF");
		return false;
	}

	return true;
}


/*
 * nan_parse_naf - Parse a NAN Action frame content
 * @nan: NAN module context from nan_init()
 * @mgmt: NAN action frame
 * @len: Length of the management frame in octets
 * @msg: Buffer for returning parsed attributes
 * Returns: 0 on success; positive or negative indicate an error
 *
 * Note: in case of success, the caller must free temporary memory allocations
 * by calling nan_attrs_clear() when the parsed data is not needed anymore. In
 * addition, as the &mgmt is referenced from the returned structure, the caller
 * must ensure that the frame buffer remains valid and unmodified as long as the
 * &msg object is used.
 */
int nan_parse_naf(struct nan_data *nan, const struct ieee80211_mgmt *mgmt,
		  size_t len, struct nan_msg *msg)
{
	if (!nan_is_naf(mgmt, len))
		return -1;

	wpa_printf(MSG_DEBUG, "NAN: Parse NAF");

	msg->oui_type = mgmt->u.action.u.naf.oui_type;
	msg->oui_subtype = mgmt->u.action.u.naf.subtype;

	msg->mgmt = mgmt;
	msg->len = len;

	return nan_parse_attrs(nan,
			       mgmt->u.action.u.naf.variable,
			       len - IEEE80211_MIN_ACTION_LEN(naf),
			       &msg->attrs);
}
