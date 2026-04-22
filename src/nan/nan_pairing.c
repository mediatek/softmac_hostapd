/*
 * Wi-Fi Aware - NAN pairing module
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include "common.h"
#include "common/ieee802_11_defs.h"
#include "pasn/pasn_common.h"
#include "nan/nan_i.h"

/**
 * nan_nira_get_tag_nonce - Generate NIRA nonce and compute NIRA tag
 * @nan: Pointer to NAN configuration structure
 * @nonce: Buffer to store the generated NIRA nonce (output)
 * @tag: Buffer to store the computed NIRA tag (output)
 * Returns: 0 on success, -1 on failure
 *
 * This function generates a random NIRA (NAN Identity Resolution Attribute)
 * nonce and derives the corresponding NIRA tag using the NIK (NAN Identity
 * Key), NMI address, and the generated nonce.
 *
 * The caller must ensure that nonce buffer is at least NAN_NIRA_NONCE_LEN bytes
 * and tag buffer is at least NAN_NIRA_TAG_LEN bytes.
 */
int nan_nira_get_tag_nonce(const struct nan_config *nan, u8 *nonce, u8 *tag)
{
	struct wpabuf *tag_buf;

	if (os_get_random(nonce, NAN_NIRA_NONCE_LEN) < 0) {
		wpa_printf(MSG_INFO, "NAN: Failed to generate NIRA nonce");
		return -1;
	}

	tag_buf = nan_crypto_derive_nira_tag(nan->nik, NAN_NIK_LEN,
					     nan->nmi_addr, nonce);
	if (!tag_buf)
		return -1;

	os_memcpy(tag, wpabuf_head(tag_buf), NAN_NIRA_TAG_LEN);
	wpabuf_free(tag_buf);

	wpa_hexdump_key(MSG_DEBUG, "NAN: NIK", nan->nik, NAN_NIK_LEN);
	wpa_hexdump(MSG_DEBUG, "NAN: NIRA-NONCE", nonce, NAN_NIRA_NONCE_LEN);
	wpa_hexdump(MSG_DEBUG, "NAN: NIRA-TAG", tag, NAN_NIRA_TAG_LEN);
	return 0;
}


/**
 * nan_pairing_add_attrs - Add NAN pairing attributes to a buffer
 * @nan: Pointer to NAN data structure containing configuration
 * @buf: Pointer to wpabuf where attributes will be added
 * Returns: 0 on success, -1 otherwise
 *
 * This function adds NAN attributes that indicate pairing capabilities
 * to the provided buffer.
 */
int nan_pairing_add_attrs(struct nan_data *nan, struct wpabuf *buf)
{
	if (!nan || !buf)
		return -1;

	nan_add_dev_capa_ext_attr(nan, buf);

	if (nan->cfg->pairing_cfg.pairing_verification) {
		if (nan_add_nira(buf, nan->nira_tag, nan->nira_nonce)) {
			wpa_printf(MSG_INFO, "NAN: Failed to add NIRA");
			return -1;
		}
	}

	return 0;
}


void nan_pairing_deinit_peer(struct nan_peer *peer)
{
	if (!peer->pairing.pasn)
		return;

	wpa_pasn_reset(peer->pairing.pasn);
	pasn_data_deinit(peer->pairing.pasn);
	peer->pairing.pasn = NULL;
	peer->pairing.self_pairing_role = NAN_PAIRING_ROLE_IDLE;
}


static bool nan_pairing_is_supported(struct nan_data *nan_data,
				     struct nan_peer *peer, u8 auth_mode)
{
	if (auth_mode == NAN_PASN_AUTH_MODE_PASN ||
	    auth_mode == NAN_PASN_AUTH_MODE_SAE) {
		if (!nan_data->cfg->pairing_cfg.pairing_setup) {
			wpa_printf(MSG_DEBUG,
				   "NAN: Pairing: Device doesn't support pairing setup");
			return false;
		}

		if (!peer->pairing.pairing_cfg.pairing_setup) {
			wpa_printf(MSG_DEBUG,
				   "NAN: Pairing: Peer doesn't support pairing setup");
			return false;
		}
	} else if (auth_mode == NAN_PASN_AUTH_MODE_PMK) {
		if (!nan_data->cfg->pairing_cfg.pairing_verification) {
			wpa_printf(MSG_DEBUG,
				   "NAN: Pairing: Device doesn't support pairing verification");
			return false;
		}

		if (!peer->pairing.pairing_cfg.pairing_verification) {
			wpa_printf(MSG_DEBUG,
				   "NAN: Pairing: Peer doesn't support pairing verification");
			return false;
		}
	}

	return true;
}


static int nan_pairing_set_password(struct pasn_data *pasn,
				    const char *passphrase)
{
#ifdef CONFIG_SAE
	struct sae_pt *pt;

	pasn->pasn_groups = os_calloc(2, sizeof(*pasn->pasn_groups));
	if (!pasn->pasn_groups) {
		wpa_printf(MSG_INFO,
			   "NAN: Pairing: Failed to allocate PASN groups");
		return -1;
	}

	pasn->pasn_groups[0] = pasn->group;

	pt = sae_derive_pt(pasn->pasn_groups, (const u8 *) NAN_PASN_SSID,
			   os_strlen(NAN_PASN_SSID), (const u8 *) passphrase,
			   os_strlen(passphrase), NULL, 0);
	if (pasn_set_pt(pasn, pt) < 0) {
		wpa_printf(MSG_INFO, "NAN: Pairing: Failed to set SAE pt");
		sae_deinit_pt(pt);
		return -1;
	}

	return 0;
#else  /* CONFIG_SAE */
	return -1;
#endif /* CONFIG_SAE */
}


static struct wpabuf * nan_pairing_generate_rsnxe(int akmp)
{
	/* According to Wi-Fi Aware Specification version 4.0, Table 26,
	 * the RSNXE's capabilities field in NAN PASN Authentication frames is
	 * 16 bits long.
	 */
	u16 capab = 1; /* bit 0-3 = Field length (n - 1) */

	struct wpabuf *buf;

	if (wpa_key_mgmt_sae(akmp))
		capab |= BIT(WLAN_RSNX_CAPAB_SAE_H2E);

	/* Element header (2 octets) + capabilities field (2 octets) */
	buf = wpabuf_alloc(4);
	if (!buf)
		return NULL;

	wpa_printf(MSG_DEBUG, "NAN: RSNXE capabilities: %04x", capab);
	wpabuf_put_u8(buf, WLAN_EID_RSNX);
	wpabuf_put_u8(buf, 2);
	wpabuf_put_le16(buf, capab);
	return buf;
}


static int nan_pairing_send_cb(void *ctx, const u8 *data, size_t data_len,
			       int noack, unsigned int freq, unsigned int wait)
{
	struct nan_data *nan_data = (struct nan_data *) ctx;

	return nan_data->cfg->send_pasn(nan_data->cfg->cb_ctx, data, data_len);
}


static int nan_pairing_pasn_initialize(struct nan_data *nan_data,
				       struct nan_peer *peer, u8 auth_mode,
				       int cipher, const char *password,
				       enum nan_pairing_role self_role)
{
	struct wpabuf *rsnxe = NULL;
	struct pasn_data *pasn;
	struct nan_pairing_peer_data *pairing;

	pairing = &peer->pairing;
	if (pairing->pasn) {
		wpa_pasn_reset(pairing->pasn);
	} else {
		pairing->pasn = pasn_data_init();
		if (!pairing->pasn) {
			wpa_printf(MSG_INFO,
				   "NAN: Pairing: Failed to initialize PASN data");
			return -1;
		}
	}

	pasn = pairing->pasn;
	pasn_set_own_addr(pasn, nan_data->cfg->nmi_addr);
	pasn_set_peer_addr(pasn, peer->nmi_addr);
	pasn_set_bssid(pasn, nan_data->cluster_id);

	if (self_role == NAN_PAIRING_ROLE_INITIATOR)
		pasn->pmksa = nan_data->initiator_pmksa;
	else
		pasn->pmksa = nan_data->responder_pmksa;

	if (cipher == WPA_CIPHER_GCMP_256 &&
	    (nan_data->cfg->pairing_cfg.cipher_suites & NAN_PAIRING_PASN_256)) {
		pasn->group = 20;
		pasn->cipher = WPA_CIPHER_GCMP_256;
	} else if (cipher == WPA_CIPHER_CCMP &&
		   (nan_data->cfg->pairing_cfg.cipher_suites &
		    NAN_PAIRING_PASN_128)) {
		pasn->group = 19;
		pasn->cipher = WPA_CIPHER_CCMP;
	} else {
		wpa_printf(MSG_INFO,
			   "NAN: Pairing: Unsupported cipher suite %s",
			   wpa_cipher_txt(cipher));
		goto fail;
	}

	pasn_enable_kdk_derivation(pasn);

	if (auth_mode == NAN_PASN_AUTH_MODE_SAE) {
		pasn_set_akmp(pasn, WPA_KEY_MGMT_SAE);
		if (!password) {
			wpa_printf(MSG_INFO,
				   "NAN: Pairing: Password not available");
			goto fail;
		}

		if (nan_pairing_set_password(pasn, password) < 0) {
			wpa_printf(MSG_INFO,
				   "NAN: Pairing: Failed to set password");
			goto fail;
		}
	} else if (auth_mode == NAN_PASN_AUTH_MODE_PASN) {
		pasn_set_akmp(pasn, WPA_KEY_MGMT_PASN);
		pasn_set_noauth(pasn, true);

		/* Set allowed PASN groups for the responder to validate */
		pasn->pasn_groups = os_calloc(2, sizeof(*pasn->pasn_groups));
		if (!pasn->pasn_groups) {
			wpa_printf(MSG_INFO,
				   "NAN: Pairing: Failed to allocate PASN groups");
			goto fail;
		}
		pasn->pasn_groups[0] = pasn->group;
	} else {
		wpa_printf(MSG_INFO,
			   "NAN: Pairing: Unsupported authentication mode %u",
			   auth_mode);
		goto fail;
	}

	pasn_set_rsn_pairwise(pasn, pasn->cipher);
	pasn_set_wpa_key_mgmt(pasn, pasn->akmp);

	if (auth_mode != NAN_PASN_AUTH_MODE_PASN) {
		rsnxe = nan_pairing_generate_rsnxe(pasn->akmp);
		if (!rsnxe) {
			wpa_printf(MSG_INFO,
				   "NAN: Pairing: Failed to generate RSNXE");
			goto fail;
		}

		pasn_set_rsnxe_ie(pairing->pasn, wpabuf_head_u8(rsnxe));
		wpabuf_free(rsnxe);
	}

	pasn_register_callbacks(pasn, nan_data, nan_pairing_send_cb, NULL,
				NULL, NULL);
	return 0;

fail:
	pasn_data_deinit(pasn);
	pairing->pasn = NULL;
	return -1;
}


/*
 * nan_pairing_prepare_pasn_elems - Prepare NAN element for pairing PASN frames
 * @nan_data: Pointer to NAN data structure
 * @peer: Pointer to NAN peer structure
 * @extra_ies: Buffer to which the NAN element is appended
 * @publish_id: Publish ID to use in the CSIA
 *
 * This function adds a NAN element containing the NAN attributes that shall be
 * included in the first and second PASN frames for NAN pairing.
 * The added attributes are:
 * - Device Capability Extension attribute (DCEA)
 * - Cipher suite information attribute (CSIA) with appropriate PASN cipher
 *   (either GCMP-256 or GCMP-128)
 * - NAN Pairing Bootstrapping Attribute (NPBA) if available
 */
static void nan_pairing_prepare_pasn_elems(struct nan_data *nan_data,
					   struct nan_peer *peer,
					   struct wpabuf *extra_ies,
					   int publish_id)
{
	u8 *len_ptr;
	struct nan_cipher_suite cs;
	size_t initial_len = wpabuf_len(extra_ies);

	wpabuf_put_u8(extra_ies, WLAN_EID_VENDOR_SPECIFIC);

	/* placeholder for length - to be filled later */
	len_ptr = wpabuf_put(extra_ies, 1);

	/* OUI + OUI Type */
	wpabuf_put_be32(extra_ies, NAN_IE_VENDOR_TYPE);

	nan_add_dev_capa_ext_attr(nan_data, extra_ies);
	if (peer->pairing.pasn->cipher == WPA_CIPHER_GCMP_256)
		cs.csid = NAN_CS_PK_PASN_256;
	else
		cs.csid = NAN_CS_PK_PASN_128;

	cs.instance_id = publish_id;

	/*
	 * TODO: Get security capabilities from somewhere. For now, it doesn't
	 * matter as the capability field is not used in pairing anyway.
	 */
	nan_add_csia(extra_ies, 0, 1, &cs);

	if (peer->bootstrap.npba)
		wpabuf_put_buf(extra_ies, peer->bootstrap.npba);

	*len_ptr = wpabuf_len(extra_ies) - initial_len - 2;
}


/**
 * nan_pairing_initiate_pasn_auth - Initiate PASN authentication for NAN pairing
 * @nan_data: NAN data context
 * @addr: MAC address of the peer device
 * @auth_mode: Authentication mode to be used (PASN, SAE, or PMK)
 * @cipher: Cipher suite to be used for the pairing
 * @handle: Handle of the service instance for which pairing is requested
 * @peer_instance_id: Instance ID of the peer service for which pairing is
 *	requested
 * @responder: Whether this device is acting as PASN responder
 * @password: Password to be used for authentication (if applicable)
 * Returns: 0 on success, -1 on failure
 */
int nan_pairing_initiate_pasn_auth(struct nan_data *nan_data, const u8 *addr,
				   u8 auth_mode, int cipher, int handle,
				   u8 peer_instance_id, bool responder,
				   const char *password)
{
	int ret = 0;
	struct pasn_data *pasn;
	struct nan_peer *peer;
	struct wpabuf *extra_ies;

	if (!addr) {
		wpa_printf(MSG_INFO, "NAN: Pairing: Peer address missing");
		return -1;
	}

	peer = nan_get_peer(nan_data, addr);
	if (!peer) {
		wpa_printf(MSG_INFO, "NAN: Pairing: Peer not known");
		return -1;
	}

	if (!nan_pairing_is_supported(nan_data, peer, auth_mode)) {
		wpa_printf(MSG_INFO,
			   "NAN: Pairing: Invalid params to initiate authentication");
		return -1;
	}

	peer->pairing.self_pairing_role = responder ?
		NAN_PAIRING_ROLE_RESPONDER : NAN_PAIRING_ROLE_INITIATOR;

	if (nan_pairing_pasn_initialize(nan_data, peer, auth_mode, cipher,
					password,
					peer->pairing.self_pairing_role)) {
		wpa_printf(MSG_INFO, "NAN: Pairing: Initialize failed");
		return -1;
	}

	pasn = peer->pairing.pasn;

	extra_ies = wpabuf_alloc(NAN_ELEMENT_MAX_SIZE);
	if (!extra_ies)
		return -1;

	/* TODO: Add support for NAN element fragmentation if it's larger than
	 * 255 octets, as defined in Wi-Fi Aware Specification v4.0 section 9.1.
	 */
	nan_pairing_prepare_pasn_elems(nan_data, peer, extra_ies, handle);
	pasn_set_extra_ies(pasn, wpabuf_head_u8(extra_ies),
			   wpabuf_len(extra_ies));
	wpabuf_free(extra_ies);

	peer->pairing.handle = handle;
	peer->pairing.peer_instance_id = peer_instance_id;

	if (responder)
		return 0;

	ret = wpas_pasn_start(pasn, pasn->own_addr, pasn->peer_addr,
			      pasn->bssid, pasn->akmp, pasn->cipher,
			      pasn->group, 0, NULL, 0, NULL, 0, NULL);
	if (ret) {
		wpa_printf(MSG_INFO, "NAN: Pairing: Failed to start PASN");
		nan_pairing_deinit_peer(peer);
	}

	return ret;
}
