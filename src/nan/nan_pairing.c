/*
 * Wi-Fi Aware - NAN pairing module
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include "common.h"
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
