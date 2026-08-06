/*
 * BSS Load Element / Channel Utilization
 * Copyright (c) 2014, Qualcomm Atheros, Inc.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "utils/eloop.h"
#include "hostapd.h"
#include "bss_load.h"
#include "ap_drv_ops.h"
#include "beacon.h"


/* Return the first BSS on the iface that has bss_load_update_period set. */
struct hostapd_data *bss_load_get_hapd(struct hostapd_iface *iface)
{
	size_t i;

	for (i = 0; i < iface->num_bss; i++) {
		if (iface->bss[i]->beacon_set_done &&
		    iface->bss[i]->conf->bss_load_update_period)
			return iface->bss[i];
	}
	return NULL;
}


static int get_bss_load_update_timeout(struct hostapd_iface *iface,
				       unsigned int *sec, unsigned int *usec)
{
	struct hostapd_data *hapd = bss_load_get_hapd(iface);
	unsigned int update_period;
	unsigned int beacon_int = iface->conf->beacon_int;
	unsigned int update_timeout;

	if (!hapd || !beacon_int) {
		wpa_printf(MSG_ERROR,
			   "BSS Load: Invalid BSS load update configuration "
			   "(no BSS configured or beacon_int=%u)",
			   beacon_int);
		return -1;
	}

	update_period = hapd->conf->bss_load_update_period;
	update_timeout = update_period * beacon_int;

	*sec = ((update_timeout / 1000) * 1024) / 1000;
	*usec = (update_timeout % 1000) * 1024;

	return 0;
}


static void update_channel_utilization(void *eloop_data, void *user_data)
{
	struct hostapd_iface *iface = eloop_data;
	struct hostapd_data *hapd;
	unsigned int sec, usec;
	size_t i;
	int err;

	/* skip if no BSS is active yet */
	for (i = 0; i < iface->num_bss; i++) {
		if (iface->bss[i]->beacon_set_done &&
		    iface->bss[i]->started)
			break;
	}
	if (i == iface->num_bss)
		return;

	hapd = bss_load_get_hapd(iface);
	if (!hapd)
		return;

	err = hostapd_drv_get_survey(iface->bss[0], iface->freq);
	if (err) {
		wpa_printf(MSG_ERROR, "BSS Load: Failed to get survey data");
		goto reschedule;
	}

	for (i = 0; i < iface->num_bss; i++) {
		if (iface->bss[i]->beacon_set_done &&
		    iface->bss[i]->conf->bss_load_update_period)
			ieee802_11_set_beacon_per_bss_only(iface->bss[i]);
	}

	if (hapd->conf->chan_util_avg_period) {
		iface->chan_util_samples_sum += iface->channel_utilization;
		iface->chan_util_num_sample_periods +=
			hapd->conf->bss_load_update_period;
		if (iface->chan_util_num_sample_periods >=
		    hapd->conf->chan_util_avg_period) {
			iface->chan_util_average =
				iface->chan_util_samples_sum /
				(iface->chan_util_num_sample_periods /
				 hapd->conf->bss_load_update_period);
			iface->chan_util_samples_sum = 0;
			iface->chan_util_num_sample_periods = 0;
		}
	}

reschedule:
	if (get_bss_load_update_timeout(iface, &sec, &usec) < 0)
		return;
	eloop_register_timeout(sec, usec, update_channel_utilization, iface,
			       NULL);
}


int bss_load_update_init(struct hostapd_iface *iface)
{
	unsigned int sec, usec;

	if (!bss_load_get_hapd(iface))
		return 0;

	if (eloop_is_timeout_registered(update_channel_utilization, iface,
					NULL))
		return 0;

	if (get_bss_load_update_timeout(iface, &sec, &usec) < 0)
		return -1;

	eloop_register_timeout(sec, usec, update_channel_utilization, iface,
			       NULL);
	return 0;
}


void bss_load_update_deinit(struct hostapd_iface *iface)
{
	eloop_cancel_timeout(update_channel_utilization, iface, NULL);
}
