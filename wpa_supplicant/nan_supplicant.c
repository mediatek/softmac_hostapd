/*
 * wpa_supplicant - NAN
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "utils/eloop.h"
#include "utils/bitfield.h"
#include "common/nan_de.h"
#include "common/ieee802_11_common.h"
#include "ap/hostapd.h"
#include "wpa_supplicant_i.h"
#include "driver_i.h"
#include "nan/nan.h"
#include "config.h"
#include "offchannel.h"
#include "notify.h"
#include "p2p_supplicant.h"
#include "pr_supplicant.h"
#include "nan_supplicant.h"

#define DEFAULT_NAN_MASTER_PREF 2
#define DEFAULT_NAN_DUAL_BAND   0
#define DEFAULT_NAN_SCAN_PERIOD 60
#define DEFAULT_NAN_SCAN_DWELL_TIME 150
#define DEFAULT_NAN_DISCOVERY_BEACON_INTERVAL 100
#define DEFAULT_NAN_LOW_BAND_FREQUENCY 2437
#define DEFAULT_NAN_HIGH_BAND_FREQUENCY 5745
#define DEFAULT_NAN_RSSI_CLOSE -50
#define DEFAULT_NAN_RSSI_MIDDLE -65

#define NAN_MIN_RSSI_CLOSE  -60
#define NAN_MIN_RSSI_MIDDLE -75

#ifdef CONFIG_NAN

static int get_center(u8 channel, const u8 *center_channels,
		      unsigned int num_chan, int width)
{
	int span = (width - 20) / 10;
	unsigned int i;

	for (i = 0; i < num_chan; i++) {
		if (channel >= center_channels[i] - span &&
		    channel <= center_channels[i] + span)
			return center_channels[i];
	}

	return 0;
}


static bool wpas_nan_valid_chan(struct wpa_supplicant *wpa_s,
				enum hostapd_hw_mode mode,
				u8 channel, int bw, u8 op_class, u8 *cf1)
{
	static const u8 nan_160mhz_5ghz_chans[] = { 50, 114, 163 };
	static const u8 nan_80mhz_5ghz_chans[] =
		{ 42, 58, 106, 122, 138, 155, 171 };
	struct hostapd_hw_modes *hw_mode;
	int width, span;
	u8 c, center = 0;

	hw_mode = get_mode(wpa_s->hw.modes, wpa_s->hw.num_modes, mode, false);
	if (!hw_mode)
		return false;

	switch (bw) {
	case BW20:
		width = 20;
		center = channel;
		break;
	case BW40PLUS:
	case BW40MINUS:
		width = 40;
		center = bw == BW40PLUS ? channel + 2 : channel - 2;
		break;
	case BW80:
		width = 80;
		center = get_center(channel, nan_80mhz_5ghz_chans,
				    ARRAY_SIZE(nan_80mhz_5ghz_chans), width);
		break;
	case BW160:
		width = 160;
		center = get_center(channel, nan_160mhz_5ghz_chans,
				    ARRAY_SIZE(nan_160mhz_5ghz_chans), width);
		break;
	default:
		return false;
	}

	if (!center)
		return false;

	span = (width - 20) / 10;
	for (c = center - span; c <= center + span; c += 4) {
		int freq = ieee80211_chan_to_freq(NULL, op_class, c);

		if (freq < 0)
			return false;

		if (ieee80211_is_dfs(freq, wpa_s->hw.modes,
				     wpa_s->hw.num_modes))
			return false;
	}

	/* Wide channels use center */
	if (width > 40)
		channel = center;

	*cf1 = center;
	return verify_channel(hw_mode, op_class, channel, bw) == ALLOWED;
}


static int wpas_nan_start_cb(void *ctx, const struct nan_cluster_config *config)
{
	struct wpa_supplicant *wpa_s = ctx;

	return wpa_drv_nan_start(wpa_s, config);
}


static int wpas_nan_update_config_cb(void *ctx,
				     const struct nan_cluster_config *config)
{
	struct wpa_supplicant *wpa_s = ctx;

	return wpa_drv_nan_update_config(wpa_s, config);
}


static void clear_sched_config(struct nan_schedule_config *sched_cfg)
{
	int i;

	for (i = 0; i < sched_cfg->num_channels; i++)
		wpabuf_free(sched_cfg->channels[i].time_bitmap);

	os_memset(sched_cfg, 0, sizeof(*sched_cfg));
}


static void wpas_nan_stop_cb(void *ctx)
{
	struct wpa_supplicant *wpa_s = ctx;
	int i;

	for (i = 0; i < MAX_NAN_RADIOS; i++) {
		if (wpa_s->nan_sched[i].num_channels) {
			wpa_drv_nan_config_schedule(wpa_s, i + 1, NULL);
			clear_sched_config(&wpa_s->nan_sched[i]);
		}
	}

	wpa_drv_nan_stop(wpa_s);
}


int wpas_nan_init(struct wpa_supplicant *wpa_s)
{
	struct nan_config nan;

	if (!(wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_SUPPORT_NAN) ||
	    !(wpa_s->nan_capa.drv_flags &
	      WPA_DRIVER_FLAGS_NAN_SUPPORT_SYNC_CONFIG)) {
		wpa_printf(MSG_INFO, "NAN: Driver does not support NAN");
		return -1;
	}

	os_memset(&nan, 0, sizeof(nan));
	nan.cb_ctx = wpa_s;

	nan.start = wpas_nan_start_cb;
	nan.stop = wpas_nan_stop_cb;
	nan.update_config = wpas_nan_update_config_cb;

	/*
	 * TODO: Set the device capabilities based on configuration and driver
	 * data. For now do not set 'n_antennas', 'channel_switch_time' and
	 * 'capa', i.e., indicating that the information is not available. This
	 * information should also be retrieved from the driver.
	 */
	nan.dev_capa.cdw_info =
		((1 << NAN_CDW_INFO_2G_POS) & NAN_CDW_INFO_2G_MASK) |
		((1 << NAN_CDW_INFO_5G_POS) & NAN_CDW_INFO_5G_MASK);

	nan.dev_capa.supported_bands = NAN_DEV_CAPA_SBAND_2G;
	if (wpa_s->nan_capa.drv_flags &
	    WPA_DRIVER_FLAGS_NAN_SUPPORT_DUAL_BAND)
		nan.dev_capa.supported_bands |= NAN_DEV_CAPA_SBAND_5G;

	nan.dev_capa.op_mode = wpa_s->nan_capa.op_modes;
	nan.dev_capa.n_antennas = wpa_s->nan_capa.num_antennas;
	nan.dev_capa.channel_switch_time =
		wpa_s->nan_capa.max_channel_switch_time;
	nan.dev_capa.capa = wpa_s->nan_capa.dev_capa;

	wpa_s->nan = nan_init(&nan);
	if (!wpa_s->nan) {
		wpa_printf(MSG_INFO, "NAN: Failed to init");
		return -1;
	}

	/* Set the default configuration */
	os_memset(&wpa_s->nan_config, 0, sizeof(wpa_s->nan_config));

	wpa_s->nan_config.master_pref = DEFAULT_NAN_MASTER_PREF;
	wpa_s->nan_config.dual_band = DEFAULT_NAN_DUAL_BAND;
	os_memset(wpa_s->nan_config.cluster_id, 0, ETH_ALEN);
	wpa_s->nan_config.scan_period = DEFAULT_NAN_SCAN_PERIOD;
	wpa_s->nan_config.scan_dwell_time = DEFAULT_NAN_SCAN_DWELL_TIME;
	wpa_s->nan_config.discovery_beacon_interval =
		DEFAULT_NAN_DISCOVERY_BEACON_INTERVAL;

	wpa_s->nan_config.low_band_cfg.frequency =
		DEFAULT_NAN_LOW_BAND_FREQUENCY;
	wpa_s->nan_config.low_band_cfg.rssi_close = DEFAULT_NAN_RSSI_CLOSE;
	wpa_s->nan_config.low_band_cfg.rssi_middle = DEFAULT_NAN_RSSI_MIDDLE;
	wpa_s->nan_config.low_band_cfg.awake_dw_interval = true;

	wpa_s->nan_config.high_band_cfg.frequency =
		DEFAULT_NAN_HIGH_BAND_FREQUENCY;
	wpa_s->nan_config.high_band_cfg.rssi_close = DEFAULT_NAN_RSSI_CLOSE;
	wpa_s->nan_config.high_band_cfg.rssi_middle = DEFAULT_NAN_RSSI_MIDDLE;
	wpa_s->nan_config.high_band_cfg.awake_dw_interval = true;

	/* TODO: Optimize this, so that the notification are enabled only when
	 * needed, i.e., when the DE is configured with unsolicited publish or
	 * active subscribe
	 */
	wpa_s->nan_config.enable_dw_notif =
		!!(wpa_s->nan_capa.drv_flags &
		   WPA_DRIVER_FLAGS_NAN_SUPPORT_USERSPACE_DE);

	return 0;
}


void wpas_nan_deinit(struct wpa_supplicant *wpa_s)
{
	int i;

	if (!wpa_s || !wpa_s->nan)
		return;

	for (i = 0; i < MAX_NAN_RADIOS; i++)
		clear_sched_config(&wpa_s->nan_sched[i]);

	nan_deinit(wpa_s->nan);
	wpa_s->nan = NULL;
}


static int wpas_nan_ready(struct wpa_supplicant *wpa_s)
{
	return wpa_s->nan_mgmt && wpa_s->nan && wpa_s->nan_de &&
		wpa_s->wpa_state != WPA_INTERFACE_DISABLED;
}


/* Join a cluster using current configuration */
int wpas_nan_start(struct wpa_supplicant *wpa_s)
{
	if (!wpas_nan_ready(wpa_s))
		return -1;

	return nan_start(wpa_s->nan, &wpa_s->nan_config);
}


int wpas_nan_stop(struct wpa_supplicant *wpa_s)
{
	if (!wpas_nan_ready(wpa_s))
		return -1;

	nan_stop(wpa_s->nan);
	nan_de_set_cluster_id(wpa_s->nan_de, NULL);

	return 0;
}


void wpas_nan_flush(struct wpa_supplicant *wpa_s)
{
	if (!wpas_nan_ready(wpa_s))
		return;

	nan_flush(wpa_s->nan);
}


int wpas_nan_set(struct wpa_supplicant *wpa_s, char *cmd)
{
	struct nan_cluster_config *config = &wpa_s->nan_config;
	char *param = os_strchr(cmd, ' ');

	if (!param)
		return -1;

	*param++ = '\0';

#define NAN_PARSE_INT(_str, _min, _max)				     \
	if (os_strcmp(#_str, cmd) == 0) {			     \
		int val = atoi(param);                               \
								     \
		if (val < (_min) || val > (_max)) {                  \
			wpa_printf(MSG_INFO,                         \
				   "NAN: Invalid value for " #_str); \
			return -1;                                   \
		}                                                    \
		config->_str = val;                                  \
		return 0;                                            \
	}

#define NAN_PARSE_BAND(_str)						\
	if (os_strcmp(#_str, cmd) == 0) {				\
		int a, b, c, d;						\
									\
		if (sscanf(param, "%d,%d,%d,%d", &a, &b, &c, &d) !=	\
		    4) {						\
			wpa_printf(MSG_DEBUG,				\
				   "NAN: Invalid value for " #_str);	\
			return -1;					\
		}							\
									\
		if (a < NAN_MIN_RSSI_CLOSE ||				\
		    b < NAN_MIN_RSSI_MIDDLE ||				\
		    a <= b) {						\
			wpa_printf(MSG_DEBUG,				\
				   "NAN: Invalid value for " #_str);	\
			return -1;					\
		}							\
		config->_str.rssi_close = a;				\
		config->_str.rssi_middle = b;				\
		config->_str.awake_dw_interval = c;			\
		config->_str.disable_scan = !!d;			\
		return 0;						\
	}

	/* 0 and 255 are reserved */
	NAN_PARSE_INT(master_pref, 1, 254);
	NAN_PARSE_INT(dual_band, 0, 1);
	NAN_PARSE_INT(scan_period, 0, 0xffff);
	NAN_PARSE_INT(scan_dwell_time, 10, 150);
	NAN_PARSE_INT(discovery_beacon_interval, 50, 200);

	NAN_PARSE_BAND(low_band_cfg);
	NAN_PARSE_BAND(high_band_cfg);

	if (os_strcmp("cluster_id", cmd) == 0) {
		u8 cluster_id[ETH_ALEN];

		if (hwaddr_aton(param, cluster_id) < 0) {
			wpa_printf(MSG_INFO, "NAN: Invalid cluster ID");
			return -1;
		}

		if (cluster_id[0] != 0x50 || cluster_id[1] != 0x6f ||
		    cluster_id[2] != 0x9a || cluster_id[3] != 0x01) {
			wpa_printf(MSG_DEBUG, "NAN: Invalid cluster ID format");
			return -1;
		}

		os_memcpy(config->cluster_id, cluster_id, ETH_ALEN);
		return 0;
	}
#undef NAN_PARSE_INT
#undef NAN_PARSE_BAND

	wpa_printf(MSG_INFO, "NAN: Unknown NAN_SET cmd='%s'", cmd);
	return -1;
}


int wpas_nan_update_conf(struct wpa_supplicant *wpa_s)
{
	if (!wpas_nan_ready(wpa_s))
		return -1;

	wpa_printf(MSG_DEBUG, "NAN: Update NAN configuration");
	return nan_update_config(wpa_s->nan, &wpa_s->nan_config);
}


static u8 nan_select_40mhz_channel(u8 chan, u8 *op_class, int *bw)
{
	int op;

	for (op = 0; global_op_class[op].op_class; op++) {
		const struct oper_class_map *o = &global_op_class[op];
		int c;

		/* No support for 40 MHz on 2.4 GHz */
		if (o->mode != HOSTAPD_MODE_IEEE80211A)
			continue;

		/* Currently don't support NAN for 80+, 6 GHz, etc. */
		if (o->op_class > 129)
			continue;

		if (o->bw != BW40MINUS && o->bw != BW40PLUS)
			continue;

		for (c = o->min_chan; c <= o->max_chan; c += o->inc) {
			if (c != chan)
				continue;

			*op_class = o->op_class;
			*bw = o->bw;
			if (o->bw == BW40MINUS)
				return chan - 2;
			else
				return chan + 2;
		}
	}

	return 0;
}


static int wpas_nan_select_channel_params(struct wpa_supplicant *wpa_s,
					  int freq, int *center_freq1,
					  int *center_freq2, int *bandwidth)
{
	u8 chan, op_class, center;
	enum hostapd_hw_mode mode;
	int bw;

	mode = ieee80211_freq_to_channel_ext(freq, 0, CONF_OPER_CHWIDTH_USE_HT,
					     &op_class, &chan);
	if (mode == NUM_HOSTAPD_MODES) {
		wpa_printf(MSG_DEBUG, "NAN: Invalid frequency %d", freq);
		return -1;
	}

	if (!wpas_nan_valid_chan(wpa_s, mode, chan, BW20, op_class, &center)) {
		wpa_printf(MSG_DEBUG,
			   "NAN: Channel not valid for NAN (freq = %d)",
			   freq);
		return -1;
	}

	/* On 2.4 GHz use 20 MHz channels */
	if (freq >= 2412 && freq <= 2484)
		goto out;

	/* TODO: Add support for NAN on other bands */
	if (freq < 5180 || freq > 5885) {
		wpa_printf(MSG_DEBUG, "NAN: Unsupported frequency %d", freq);
		return -1;
	}

	if (wpas_nan_valid_chan(wpa_s, mode, chan, BW160, 129, &center)) {
		*center_freq1 = ieee80211_chan_to_freq(NULL, op_class, center);
		*center_freq2 = 0;
		*bandwidth = 160;
		return 0;
	}

	if (wpas_nan_valid_chan(wpa_s, mode, chan, BW80, 128, &center)) {
		*center_freq1 = ieee80211_chan_to_freq(NULL, op_class, center);
		*center_freq2 = 0;
		*bandwidth = 80;
		return 0;
	}

	if (nan_select_40mhz_channel(chan, &op_class, &bw) &&
		wpas_nan_valid_chan(wpa_s, mode, center, bw, op_class,
				    &center)) {
		*center_freq1 = ieee80211_chan_to_freq(NULL, op_class,
						       center);
		*center_freq2 = 0;
		*bandwidth = 40;
		return 0;
	}

out:
	/* Fallback to 20 MHz */
	*center_freq1 = freq;
	*center_freq2 = 0;
	*bandwidth = 20;
	return 0;
}


static void nan_dump_sched_config(const char *title,
				  struct nan_schedule_config *sched_cfg)
{
	int i;

	wpa_printf(MSG_DEBUG, "%s: num_channels=%d", title,
		   sched_cfg->num_channels);
	for (i = 0; i < sched_cfg->num_channels; i++) {
		wpa_printf(MSG_DEBUG,
			   "  Channel %d: freq=%d center_freq1=%d center_freq2=%d bandwidth=%d time_bitmap_len=%zu",
			   i + 1,
			   sched_cfg->channels[i].freq,
			   sched_cfg->channels[i].center_freq1,
			   sched_cfg->channels[i].center_freq2,
			   sched_cfg->channels[i].bandwidth,
			   wpabuf_len(sched_cfg->channels[i].time_bitmap));
	}
}


/* Parse format NAN_SCHED_CONFIG_MAP map_id=<id> [freq:bitmap_hex]..
 * If no bitmaps provided - clear the map */
int wpas_nan_sched_config_map(struct wpa_supplicant *wpa_s, const char *cmd)
{
	struct nan_schedule_config sched_cfg;
	char *token, *context = NULL;
	u8 map_id;
	char *pos;
	int *shared_freqs;
	int shared_freqs_count, unused_freqs_count, ret = -1;
	struct bitfield *bf_total;
	unsigned int expected_bitmap_len;

	if (!wpas_nan_ready(wpa_s))
		return -1;

	if (os_strncmp(cmd, "map_id=", 7) != 0) {
		wpa_printf(MSG_INFO, "NAN: Invalid schedule map format");
		return -1;
	}

	map_id = atoi(cmd + 7);

	if (!map_id || map_id >= MAX_NAN_RADIOS) {
		wpa_printf(MSG_INFO, "NAN: Invalid map_id %d", map_id);
		return -1;
	}

	if (map_id > wpa_s->nan_capa.num_radios) {
		wpa_printf(MSG_INFO,
			   "NAN: map_id %d exceeds number of supported NAN radios %d",
			   map_id, wpa_s->nan_capa.num_radios);
		return -1;
	}

	if (!wpa_s->nan_capa.schedule_period ||
	    !wpa_s->nan_capa.slot_duration) {
		    wpa_printf(MSG_INFO,
			       "NAN: Driver doesn't advertise support for NAN scheduling");
		    return -1;
	}

	expected_bitmap_len = (wpa_s->nan_capa.schedule_period /
			       wpa_s->nan_capa.slot_duration + 7) / 8;

	os_memset(&sched_cfg, 0, sizeof(sched_cfg));

	pos = os_strchr(cmd + 7, ' ');
	if (!pos) {
		clear_sched_config(&wpa_s->nan_sched[map_id - 1]);
		wpa_printf(MSG_INFO,
			   "NAN: Missing freq:timebitmap pairs - cleanup schedule");
		return wpa_drv_nan_config_schedule(wpa_s, map_id, &sched_cfg);
	}

	shared_freqs = os_calloc(wpa_s->num_multichan_concurrent,
				 sizeof(int));
	if (!shared_freqs) {
		wpa_printf(MSG_INFO,
			   "NAN: Failed to allocate memory for shared freqs");
		return -1;
	}

	shared_freqs_count =
		get_shared_radio_freqs(wpa_s, shared_freqs,
				       wpa_s->num_multichan_concurrent,
				       false);

	unused_freqs_count = wpa_s->nan_capa.sched_chans - shared_freqs_count;

	bf_total = bitfield_alloc(wpa_s->nan_capa.schedule_period /
				  wpa_s->nan_capa.slot_duration);
	if (!bf_total) {
		wpa_printf(MSG_INFO,
			  "NAN: Failed to allocate bitfield for total schedule");
		goto out;
	}

	/* Parse freq:timebitmap pairs */
	pos++;
	while ((token = str_token(pos, " ", &context))) {
		int j, i = sched_cfg.num_channels;;
		struct bitfield *bf_chan = NULL;
		char *colon = os_strchr(token, ':');

		if (i >= wpa_s->nan_capa.sched_chans) {
			wpa_printf(MSG_INFO,
				   "NAN: Exceeded max channels per radio %u",
				   wpa_s->nan_capa.sched_chans);
			goto out;
		}

		if (!colon) {
			wpa_printf(MSG_INFO,
				   "NAN: Invalid freq:timebitmap format");
			goto out;
		}

		sched_cfg.channels[i].freq = atoi(token);
		if (sched_cfg.channels[i].freq <= 0) {
			wpa_printf(MSG_INFO, "NAN: Invalid frequency %d",
				   sched_cfg.channels[i].freq);
			goto out;
		}

		for (j = 0; j < i; j++) {
			if (sched_cfg.channels[j].freq ==
			    sched_cfg.channels[i].freq) {
				wpa_printf(MSG_INFO,
					   "NAN: Duplicate frequency %d",
					   sched_cfg.channels[i].freq);
				goto out;
			}
		}

		if (wpas_nan_select_channel_params(
			    wpa_s, sched_cfg.channels[i].freq,
			    &sched_cfg.channels[i].center_freq1,
			    &sched_cfg.channels[i].center_freq2,
			    &sched_cfg.channels[i].bandwidth)) {
			wpa_printf(MSG_INFO,
				   "NAN: Failed to select channel params for freq %d",
				   sched_cfg.channels[i].freq);
			goto out;
		}

		if (!int_array_includes(shared_freqs,
					sched_cfg.channels[i].freq)) {
			if (!unused_freqs_count) {
				wpa_printf(MSG_INFO,
					   "NAN: No unused radio frequency available for freq %d",
					   sched_cfg.channels[i].freq);
				goto out;
			}

			unused_freqs_count--;
		}

		sched_cfg.channels[i].time_bitmap = wpabuf_parse_bin(colon + 1);
		if (!sched_cfg.channels[i].time_bitmap) {
			wpa_printf(MSG_INFO, "NAN: Invalid time bitmap");
			goto out;
		}

		sched_cfg.num_channels++;

		if (wpabuf_len(sched_cfg.channels[i].time_bitmap) !=
		    expected_bitmap_len) {
			wpa_printf(MSG_INFO,
				   "NAN: Invalid bitmap length (%zu) for period=%d, slot length=%d",
				   wpabuf_len(sched_cfg.channels[i].time_bitmap),
				   wpa_s->nan_capa.schedule_period,
				   wpa_s->nan_capa.slot_duration);
			goto out;
		}

		bf_chan = bitfield_alloc_data(
			wpabuf_head(sched_cfg.channels[i].time_bitmap),
			wpabuf_len(sched_cfg.channels[i].time_bitmap));
		if (!bf_chan) {
			wpa_printf(MSG_INFO,
				   "NAN: Failed to allocate bitfield for channel schedule");
			goto out;
		}

		if (bitfield_intersects(bf_total, bf_chan)) {
			wpa_printf(MSG_INFO,
				   "NAN: Overlapping time bitmap detected for freq %d",
				   sched_cfg.channels[i].freq);
			bitfield_free(bf_chan);
			goto out;
		}

		/* Extract RX NSS from upper nibble of num_antennas */
		sched_cfg.channels[i].rx_nss =
			(wpa_s->nan_capa.num_antennas >> 4) & 0x0f;

		bitfield_union_in_place(bf_total, bf_chan);
		bitfield_free(bf_chan);
	}

	nan_dump_sched_config("NAN: Set schedule config", &sched_cfg);
	ret = wpa_drv_nan_config_schedule(wpa_s, map_id, &sched_cfg);
	if (ret < 0) {
		wpa_printf(MSG_INFO,
			   "NAN: Failed to configure NAN schedule map_id %d",
			   map_id);
		goto out;
	}

	/* Store the configured schedule */
	wpa_s->schedule_sequence_id++;
	clear_sched_config(&wpa_s->nan_sched[map_id - 1]);
	os_memcpy(&wpa_s->nan_sched[map_id - 1], &sched_cfg, sizeof(sched_cfg));
out:
	os_free(bf_total);
	os_free(shared_freqs);
	if (ret)
		clear_sched_config(&sched_cfg);

	return ret;
}


static struct wpabuf * wpas_nan_build_ndp_elems(struct wpa_supplicant *wpa_s)
{
	struct ieee80211_ht_capabilities *ht_cap;
	struct ieee80211_vht_capabilities *vht_cap;
	size_t len;
	struct wpabuf *buf;

	/* Include HT and VHT Capability elements */
	len = 2 + sizeof(struct ieee80211_ht_capabilities);
	if (wpa_s->nan_capa.vht_valid)
		len += 2 + sizeof(struct ieee80211_vht_capabilities);

	buf = wpabuf_alloc(len);
	if (!buf)
		return NULL;

	wpabuf_put_u8(buf, WLAN_EID_HT_CAP);
	wpabuf_put_u8(buf, sizeof(*ht_cap));
	ht_cap = wpabuf_put(buf, sizeof(*ht_cap));
	ht_cap->ht_capabilities_info = host_to_le16(wpa_s->nan_capa.ht_capab);
	ht_cap->a_mpdu_params = wpa_s->nan_capa.ht_ampdu_params;
	os_memcpy(ht_cap->supported_mcs_set, wpa_s->nan_capa.ht_mcs_set,
		  sizeof(ht_cap->supported_mcs_set));

	if (!wpa_s->nan_capa.vht_valid)
		return buf;

	wpabuf_put_u8(buf, WLAN_EID_VHT_CAP);
	wpabuf_put_u8(buf, sizeof(*vht_cap));
	vht_cap = wpabuf_put(buf, sizeof(*vht_cap));
	vht_cap->vht_capabilities_info =
		host_to_le32(wpa_s->nan_capa.vht_capab);
	os_memcpy(&vht_cap->vht_supported_mcs_set,
		  wpa_s->nan_capa.vht_mcs_set,
		  sizeof(vht_cap->vht_supported_mcs_set));

	/* TODO: Add HE capabilities */
	return buf;
}


static int
wpas_nan_fill_ndp_schedule_chan(struct wpa_supplicant *wpa_s,
				struct nan_schedule *sched, int map_id,
				const struct nan_schedule_channel *chan)
{
	struct nan_chan_schedule *chan_sched;
	const u8 *bitmap_data;
	size_t bitmap_len;

	/* None of these should happen */
	if (!chan->time_bitmap) {
		wpa_printf(MSG_INFO,
			   "NAN: Missing time bitmap for map_id %d freq %d",
			   map_id + 1, chan->freq);
		return -1;
	}

	bitmap_len = wpabuf_len(chan->time_bitmap);
	bitmap_data = wpabuf_head(chan->time_bitmap);
	if (bitmap_len > NAN_TIME_BITMAP_MAX_LEN) {
		wpa_printf(MSG_INFO,
			   "NAN: Time bitmap length %zu exceeds maximum %d",
			   bitmap_len, NAN_TIME_BITMAP_MAX_LEN);
		return -1;
	}

	chan_sched = &sched->chans[sched->n_chans++];
	chan_sched->map_id = map_id + 1;
	chan_sched->chan.freq = chan->freq;
	chan_sched->chan.center_freq1 = chan->center_freq1;
	chan_sched->chan.center_freq2 = chan->center_freq2;
	chan_sched->chan.bandwidth = chan->bandwidth;

	chan_sched->committed.duration = wpa_s->nan_capa.slot_duration >> 5;
	chan_sched->committed.period = ffs(wpa_s->nan_capa.schedule_period) - 7;
	chan_sched->committed.offset = 0;
	chan_sched->committed.len = bitmap_len;
	os_memcpy(chan_sched->committed.bitmap, bitmap_data, bitmap_len);
	wpa_printf(MSG_DEBUG,
		   "NAN: NDP schedule channel added: map_id=%d freq=%d center_freq1=%d center_freq2=%d bandwidth=%d",
		   chan_sched->map_id,
		   chan_sched->chan.freq,
		   chan_sched->chan.center_freq1,
		   chan_sched->chan.center_freq2,
		   chan_sched->chan.bandwidth);

	return 0;
}


static void wpas_nan_fill_ndp_schedule(struct wpa_supplicant *wpa_s,
				       struct nan_schedule *sched)
{
	int map_id;

	os_memset(sched, 0, sizeof(*sched));

	/* Fill the NAN schedule structure from the schedule config */
	for (map_id = 0; map_id < MAX_NAN_RADIOS; map_id++) {
		int i;
		struct nan_schedule_config *sched_cfg =
			&wpa_s->nan_sched[map_id];

		for (i = 0; i < wpa_s->nan_sched[map_id].num_channels; i++) {
			struct nan_schedule_channel *chan;

			chan = &sched_cfg->channels[i];
			if (wpas_nan_fill_ndp_schedule_chan(wpa_s, sched,
							    map_id, chan)
			    < 0)
				return;
		}
	}

	/* Mark all supported radios - for potential availability */
	sched->map_ids_bitmap = (BIT(wpa_s->nan_capa.num_radios) - 1) << 1;
}


static int wpas_nan_get_ndc_map_id(struct wpa_supplicant *wpa_s,
				   const struct nan_peer_schedule *peer_sched,
				   u8 peer_map_id)
{
	int i;
	int freq = nan_get_peer_ndc_freq(wpa_s->nan, peer_sched, peer_map_id);

	if (freq < 0) {
		wpa_printf(MSG_DEBUG,
			   "NAN: Failed to get NDC frequency from peer schedule");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "NAN: Peer NDC frequency is %d MHz", freq);

	for (i = 0; i < MAX_NAN_RADIOS; i++) {
		struct nan_schedule_config *sched_cfg = &wpa_s->nan_sched[i];
		int j;

		for (j = 0; j < sched_cfg->num_channels; j++) {
			if (sched_cfg->channels[j].freq == freq) {
				wpa_printf(MSG_DEBUG,
					   "NAN: Found local NDC map_id %d for peer NDC freq %d",
					   i + 1, freq);
				return i + 1;
			}
		}
	}

	return -1;
}



static int wpas_nan_select_ndc_copy_peers(struct wpa_supplicant *wpa_s,
					  struct nan_ndp_params *ndp)
{
	struct nan_peer_schedule peer_sched;
	int ret;
	u8 map_id;

	wpa_printf(MSG_DEBUG, "NAN: NDP CONF - use the NDC from peer");
	ret = nan_peer_get_schedule_info(wpa_s->nan, ndp->ndp_id.peer_nmi,
					 &peer_sched);
	if (ret) {
		wpa_printf(MSG_DEBUG, "NAN: Failed to get peer schedule info");
		return -1;
	}

	for (map_id = 0; map_id < peer_sched.n_maps; map_id++) {
		if (peer_sched.maps[map_id].ndc.len) {
			ret = wpas_nan_get_ndc_map_id(wpa_s, &peer_sched,
						      map_id);
			if (ret < 0) {
				wpa_printf(MSG_DEBUG,
					   "NAN: No local NDC map_id found for peer NDC");
				return -1;
			}

			ndp->sched.ndc_map_id = ret;
			os_memcpy(&ndp->sched.ndc, &peer_sched.maps[map_id].ndc,
				  sizeof(ndp->sched.ndc));
			return 0;
		}
	}

	wpa_printf(MSG_DEBUG, "NAN: No NDC found in peer schedule");
	return -1;
}


static int wpas_nan_select_ndc(struct wpa_supplicant *wpa_s,
			       struct nan_ndp_params *ndp)
{
	int i;

	/* NDC attribute in request is optional, let the peer decide */
	if (ndp->type == NAN_NDP_ACTION_REQ)
		return 0;

	/* For successfull confirm, copy peer's NDC */
	if (ndp->type == NAN_NDP_ACTION_CONF &&
	    ndp->u.resp.status == NAN_NDP_STATUS_ACCEPTED)
		return wpas_nan_select_ndc_copy_peers(wpa_s, ndp);

	os_memcpy(&ndp->sched.ndc, &ndp->sched.chans[0].committed,
		  sizeof(ndp->sched.ndc));
	os_memset(ndp->sched.ndc.bitmap, 0, sizeof(ndp->sched.ndc.bitmap));
	ndp->sched.ndc_map_id = ndp->sched.chans[0].map_id;

	/*
	 * For default NDC channels (6, 149, 44) take the first slot after DW.
	 * Note that if the slot duration is 16 TUs we need to select the next
	 * slot after DW. If the first channel is not one of default NDC
	 * channels, select the first available slot.
	 */
	if (ndp->sched.chans[0].chan.freq == 5745 ||
	    ndp->sched.chans[0].chan.freq == 5220) {
		int dw_bit, byte_idx, bit_in_byte;

		dw_bit = 128 / wpa_s->nan_capa.slot_duration;
		dw_bit += !!(wpa_s->nan_capa.slot_duration == 16);
		byte_idx = dw_bit / 8;
		bit_in_byte = dw_bit % 8;

		if (ndp->sched.chans[0].committed.bitmap[byte_idx] &
		    BIT(bit_in_byte)) {
			ndp->sched.ndc.bitmap[byte_idx] = BIT(bit_in_byte);
			return 0;
		}
	} else if (ndp->sched.chans[0].chan.freq == 2437 &&
		   wpa_s->nan_capa.slot_duration == 16) {
		if (ndp->sched.chans[0].committed.bitmap[0] & 0x02) {
			ndp->sched.ndc.bitmap[0] = 0x02;
			return 0;
		}
	}

	/* For other cases, select the first available slot */
	for (i = 0; i < NAN_TIME_BITMAP_MAX_LEN; i++) {
		if (ndp->sched.chans[0].committed.bitmap[i]) {
			ndp->sched.ndc.bitmap[i] =
				ndp->sched.chans[0].committed.bitmap[i] &
				(~ndp->sched.chans[0].committed.bitmap[i] + 1);
			break;
		}
	}

	return 0;
}


static int wpas_nan_set_ndp_schedule(struct wpa_supplicant *wpa_s,
				     struct nan_ndp_params *ndp)
{
	/* Set schedule for request or successful response */
	if (ndp->type != NAN_NDP_ACTION_REQ &&
	    ndp->u.resp.status == NAN_NDP_STATUS_REJECTED)
		return 0;

	wpas_nan_fill_ndp_schedule(wpa_s, &ndp->sched);

	if (!ndp->sched.n_chans) {
		wpa_printf(MSG_DEBUG,
			   "NAN: No channels configured for NDP schedule");
		return -1;
	}

	/* Set sequence ID */
	ndp->sched.sequence_id = wpa_s->schedule_sequence_id;

	/* Add additional elements */
	ndp->sched.elems = wpas_nan_build_ndp_elems(wpa_s);

	/* Mark schedule as valid */
	ndp->sched_valid = true;

	return wpas_nan_select_ndc(wpa_s, ndp);
}


/* Command format NAN_NDP_REQUEST handle=<id> ndi=<ifname> peer_nmi=<nmi>
   peer_id=<peer_instance_id> ssi=<hexdata> qos=<slots:latency> */
int wpas_nan_ndp_request(struct wpa_supplicant *wpa_s, char *cmd)
{
	struct nan_ndp_params ndp;
	struct wpabuf *ssi_buf = NULL;
	char *token, *context = NULL;
	char *pos;
	int handle = -1;
	int ret = -1;

	os_memset(&ndp, 0, sizeof(ndp));

	if (!wpas_nan_ready(wpa_s))
		return -1;

	ndp.type = NAN_NDP_ACTION_REQ;
	ndp.qos.min_slots = NAN_QOS_MIN_SLOTS_NO_PREF;
	ndp.qos.max_latency = NAN_QOS_MAX_LATENCY_NO_PREF;

	/* Parse command parameters */
	while ((token = str_token(cmd, " ", &context))) {
		pos = os_strchr(token, '=');
		if (!pos) {
			wpa_printf(MSG_INFO,
				   "NAN: Invalid parameter format: %s",
				   token);
			goto fail;
		}
		*pos++ = '\0';

		if (os_strcmp(token, "handle") == 0) {
			handle = atoi(pos);

			/* Get service ID from the local handle */
			if (!nan_de_is_valid_instance_id(wpa_s->nan_de,
							 handle, false,
							 ndp.u.req.service_id))
			{
				wpa_printf(MSG_INFO,
					   "NAN: Invalid subscribe handle: %d",
					   handle);
				goto fail;
			}
		} else if (os_strcmp(token, "ndi") == 0) {
			struct wpa_supplicant *ndi_wpa_s;

			ndi_wpa_s = wpa_supplicant_get_iface(wpa_s->global,
							     pos);
			if (!ndi_wpa_s) {
				wpa_printf(MSG_INFO,
					   "NAN: NDI interface not found: %s",
					   pos);
				goto fail;
			}

			if (!ndi_wpa_s->nan_data) {
				wpa_printf(MSG_INFO,
					   "NAN: Interface %s is not a NAN data interface",
					   pos);
				goto fail;
			}

			os_memcpy(ndp.ndp_id.init_ndi, ndi_wpa_s->own_addr,
				  ETH_ALEN);
		} else if (os_strcmp(token, "peer_nmi") == 0) {
			if (hwaddr_aton(pos, ndp.ndp_id.peer_nmi) < 0) {
				wpa_printf(MSG_INFO,
					   "NAN: Invalid peer NMI address: %s",
					   pos);
				goto fail;
			}

		} else if (os_strcmp(token, "peer_id") == 0) {
			ndp.u.req.publish_inst_id = atoi(pos);
		} else if (os_strcmp(token, "ssi") == 0) {
			ssi_buf = wpabuf_parse_bin(pos);
			if (!ssi_buf) {
				wpa_printf(MSG_INFO,
					   "NAN: Invalid SSI data: %s", pos);
				goto fail;
			}

			ndp.ssi_len = wpabuf_len(ssi_buf);
			ndp.ssi = wpabuf_head(ssi_buf);
		} else if (os_strcmp(token, "qos") == 0) {
			if (sscanf(pos, "%hhu:%hu",
				   &ndp.qos.min_slots,
				   &ndp.qos.max_latency) != 2) {
				wpa_printf(MSG_INFO,
					   "NAN: Invalid QoS parameter: %s",
					   pos);
				goto fail;
			}
		} else {
			wpa_printf(MSG_INFO, "NAN: Unknown parameter: %s",
				   token);
			goto fail;
		}
	}

	/* Validate required parameters */
	if (handle < 0) {
		wpa_printf(MSG_INFO, "NAN: Missing required parameter: handle");
		goto fail;
	}

	if (!ndp.u.req.publish_inst_id) {
		wpa_printf(MSG_INFO,
			   "NAN: Missing required parameter: peer_id");
		goto fail;
	}

	if (is_zero_ether_addr(ndp.ndp_id.init_ndi)) {
		wpa_printf(MSG_INFO, "NAN: Missing required parameter: ndi");
		goto fail;
	}

	if (is_zero_ether_addr(ndp.ndp_id.peer_nmi)) {
		wpa_printf(MSG_INFO,
			   "NAN: Missing required parameter: peer_nmi");
		goto fail;
	}

	if (wpas_nan_set_ndp_schedule(wpa_s, &ndp)) {
		wpa_printf(MSG_INFO, "NAN: Failed to set NDP schedule");
		goto fail;
	}

	wpa_printf(MSG_DEBUG, "NAN: Requesting NDP with peer " MACSTR
		   " using handle %d", MAC2STR(ndp.ndp_id.peer_nmi),
		   ndp.u.req.publish_inst_id);
	ret = nan_handle_ndp_setup(wpa_s->nan, &ndp);
fail:
	wpabuf_free(ndp.sched.elems);
	wpabuf_free(ssi_buf);

	return ret;
}


void wpas_nan_cluster_join(struct wpa_supplicant *wpa_s,
			   const u8 *cluster_id,
			   bool new_cluster)
{
	if (!wpas_nan_ready(wpa_s))
		return;

	wpa_msg_global(wpa_s, MSG_INFO, NAN_CLUSTER_JOIN "cluster_id=" MACSTR
		       " new=%d", MAC2STR(cluster_id), new_cluster);

	nan_de_set_cluster_id(wpa_s->nan_de, cluster_id);
	nan_set_cluster_id(wpa_s->nan, cluster_id);
}


void wpas_nan_next_dw(struct wpa_supplicant *wpa_s, u32 freq)
{
	if (!wpas_nan_ready(wpa_s))
		return;

	wpa_printf(MSG_DEBUG, "NAN: Next DW notification freq=%d", freq);
	nan_de_dw_trigger(wpa_s->nan_de, freq);
}

#endif /* CONFIG_NAN */


static const char *
tx_status_result_txt(enum offchannel_send_action_result result)
{
	switch (result) {
	case OFFCHANNEL_SEND_ACTION_SUCCESS:
		return "success";
	case OFFCHANNEL_SEND_ACTION_NO_ACK:
		return "no-ack";
	case OFFCHANNEL_SEND_ACTION_FAILED:
		return "failed";
	}

	return "?";
}


static void wpas_nan_de_tx_status(struct wpa_supplicant *wpa_s,
				  unsigned int freq, const u8 *dst,
				  const u8 *src, const u8 *bssid,
				  const u8 *data, size_t data_len,
				  enum offchannel_send_action_result result)
{
	if (!wpa_s->nan_de)
		return;

	wpa_printf(MSG_DEBUG, "NAN: TX status A1=" MACSTR " A2=" MACSTR
		   " A3=" MACSTR " freq=%d len=%zu result=%s",
		   MAC2STR(dst), MAC2STR(src), MAC2STR(bssid), freq,
		   data_len, tx_status_result_txt(result));

	nan_de_tx_status(wpa_s->nan_de, freq, dst);
}


struct wpas_nan_usd_tx_work {
	unsigned int freq;
	unsigned int wait_time;
	u8 dst[ETH_ALEN];
	u8 src[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	struct wpabuf *buf;
};


static void wpas_nan_usd_tx_work_free(struct wpas_nan_usd_tx_work *twork)
{
	if (!twork)
		return;
	wpabuf_free(twork->buf);
	os_free(twork);
}


static void wpas_nan_usd_tx_work_done(struct wpa_supplicant *wpa_s)
{
	struct wpas_nan_usd_tx_work *twork;

	if (!wpa_s->nan_usd_tx_work)
		return;

	twork = wpa_s->nan_usd_tx_work->ctx;
	wpas_nan_usd_tx_work_free(twork);
	radio_work_done(wpa_s->nan_usd_tx_work);
	wpa_s->nan_usd_tx_work = NULL;
}


static int wpas_nan_de_tx_send(struct wpa_supplicant *wpa_s, unsigned int freq,
			       unsigned int wait_time, const u8 *dst,
			       const u8 *src, const u8 *bssid,
			       const struct wpabuf *buf)
{
	wpa_printf(MSG_DEBUG, "NAN: TX NAN SDF A1=" MACSTR " A2=" MACSTR
		   " A3=" MACSTR " freq=%d len=%zu",
		   MAC2STR(dst), MAC2STR(src), MAC2STR(bssid), freq,
		   wpabuf_len(buf));

	return offchannel_send_action(wpa_s, freq, dst, src, bssid,
				      wpabuf_head(buf), wpabuf_len(buf),
				      wait_time, wpas_nan_de_tx_status, 1);
}


static void wpas_nan_usd_start_tx_cb(struct wpa_radio_work *work, int deinit)
{
	struct wpa_supplicant *wpa_s = work->wpa_s;
	struct wpas_nan_usd_tx_work *twork = work->ctx;

	if (deinit) {
		if (work->started) {
			wpa_s->nan_usd_tx_work = NULL;
			offchannel_send_action_done(wpa_s);
		}
		wpas_nan_usd_tx_work_free(twork);
		return;
	}

	wpa_s->nan_usd_tx_work = work;

	if (wpas_nan_de_tx_send(wpa_s, twork->freq, twork->wait_time,
				twork->dst, twork->src, twork->bssid,
				twork->buf) < 0)
		wpas_nan_usd_tx_work_done(wpa_s);
}


static int wpas_nan_de_tx(void *ctx, unsigned int freq, unsigned int wait_time,
			  const u8 *dst, const u8 *src, const u8 *bssid,
			  const struct wpabuf *buf)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct wpas_nan_usd_tx_work *twork;

	if (!freq && !wait_time) {
		int ret;

		wpa_printf(MSG_DEBUG, "NAN: SYNC TX NAN SDF A1=" MACSTR " A2="
			   MACSTR " A3=" MACSTR " len=%zu",
			   MAC2STR(dst), MAC2STR(src), MAC2STR(bssid),
			   wpabuf_len(buf));
		ret = wpa_drv_send_action(wpa_s, 0, 0, dst, src, bssid,
					  wpabuf_head(buf), wpabuf_len(buf),
					  1);
		if (ret)
			wpa_printf(MSG_DEBUG,
				   "NAN: Failed to send sync action frame (%d)",
				   ret);
		return ret;
	}

	if (wpa_s->nan_usd_tx_work || wpa_s->nan_usd_listen_work) {
		/* Reuse ongoing radio work */
		return wpas_nan_de_tx_send(wpa_s, freq, wait_time, dst, src,
					   bssid, buf);
	}

	twork = os_zalloc(sizeof(*twork));
	if (!twork)
		return -1;
	twork->freq = freq;
	twork->wait_time = wait_time;
	os_memcpy(twork->dst, dst, ETH_ALEN);
	os_memcpy(twork->src, src, ETH_ALEN);
	os_memcpy(twork->bssid, bssid, ETH_ALEN);
	twork->buf = wpabuf_dup(buf);
	if (!twork->buf) {
		wpas_nan_usd_tx_work_free(twork);
		return -1;
	}

	if (!radio_add_work(wpa_s, freq, "nan-usd-tx", 0,
			    wpas_nan_usd_start_tx_cb, twork)) {
		wpas_nan_usd_tx_work_free(twork);
		return -1;
	}

	return 0;
}


struct wpas_nan_usd_listen_work {
	unsigned int freq;
	unsigned int duration;
};


static void wpas_nan_usd_listen_work_done(struct wpa_supplicant *wpa_s)
{
	struct wpas_nan_usd_listen_work *lwork;

	if (!wpa_s->nan_usd_listen_work)
		return;

	lwork = wpa_s->nan_usd_listen_work->ctx;
	os_free(lwork);
	radio_work_done(wpa_s->nan_usd_listen_work);
	wpa_s->nan_usd_listen_work = NULL;
}


static void wpas_nan_usd_remain_on_channel_timeout(void *eloop_ctx,
						   void *timeout_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	struct wpas_nan_usd_listen_work *lwork = timeout_ctx;

	wpas_nan_usd_cancel_remain_on_channel_cb(wpa_s, lwork->freq);
}


static void wpas_nan_usd_start_listen_cb(struct wpa_radio_work *work,
					 int deinit)
{
	struct wpa_supplicant *wpa_s = work->wpa_s;
	struct wpas_nan_usd_listen_work *lwork = work->ctx;
	unsigned int duration;

	if (deinit) {
		if (work->started) {
			wpa_s->nan_usd_listen_work = NULL;
			wpa_drv_cancel_remain_on_channel(wpa_s);
		}
		os_free(lwork);
		return;
	}

	wpa_s->nan_usd_listen_work = work;

	duration = lwork->duration;
	if (duration > wpa_s->max_remain_on_chan)
		duration = wpa_s->max_remain_on_chan;
	wpa_printf(MSG_DEBUG, "NAN: Start listen on %u MHz for %u ms",
		   lwork->freq, duration);
	if (wpa_drv_remain_on_channel(wpa_s, lwork->freq, duration) < 0) {
		wpa_printf(MSG_DEBUG,
			   "NAN: Failed to request the driver to remain on channel (%u MHz) for listen",
			   lwork->freq);
		eloop_cancel_timeout(wpas_nan_usd_remain_on_channel_timeout,
				     wpa_s, ELOOP_ALL_CTX);
		/* Restart the listen state after a delay */
		eloop_register_timeout(0, 500,
				       wpas_nan_usd_remain_on_channel_timeout,
				       wpa_s, lwork);
		wpas_nan_usd_listen_work_done(wpa_s);
		return;
	}
}


static int wpas_nan_de_listen(void *ctx, unsigned int freq,
			      unsigned int duration)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct wpas_nan_usd_listen_work *lwork;

	lwork = os_zalloc(sizeof(*lwork));
	if (!lwork)
		return -1;
	lwork->freq = freq;
	lwork->duration = duration;

	if (!radio_add_work(wpa_s, freq, "nan-usd-listen", 0,
			    wpas_nan_usd_start_listen_cb, lwork)) {
		os_free(lwork);
		return -1;
	}

	return 0;
}


static void
wpas_nan_de_discovery_result(void *ctx, int subscribe_id,
			     enum nan_service_protocol_type srv_proto_type,
			     const u8 *ssi, size_t ssi_len, int peer_publish_id,
			     const u8 *peer_addr, bool fsd, bool fsd_gas)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_notify_nan_discovery_result(wpa_s, srv_proto_type, subscribe_id,
					 peer_publish_id, peer_addr, fsd,
					 fsd_gas, ssi, ssi_len);
}


static void wpas_nan_de_replied(void *ctx, int publish_id, const u8 *peer_addr,
				int peer_subscribe_id,
				enum nan_service_protocol_type srv_proto_type,
				const u8 *ssi, size_t ssi_len)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_notify_nan_replied(wpa_s, srv_proto_type, publish_id,
				peer_subscribe_id, peer_addr, ssi, ssi_len);
}


static void wpas_nan_de_publish_terminated(void *ctx, int publish_id,
					   enum nan_de_reason reason)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_notify_nan_publish_terminated(wpa_s, publish_id, reason);
}


static void wpas_nan_usd_offload_cancel_publish(void *ctx, int publish_id)
{
	struct wpa_supplicant *wpa_s = ctx;

	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		wpas_drv_nan_cancel_publish(wpa_s, publish_id);
}


static void wpas_nan_de_subscribe_terminated(void *ctx, int subscribe_id,
					     enum nan_de_reason reason)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_notify_nan_subscribe_terminated(wpa_s, subscribe_id, reason);
}


static void wpas_nan_usd_offload_cancel_subscribe(void *ctx, int subscribe_id)
{
	struct wpa_supplicant *wpa_s = ctx;

	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		wpas_drv_nan_cancel_subscribe(wpa_s, subscribe_id);
}


static void wpas_nan_de_receive(void *ctx, int id, int peer_instance_id,
				const u8 *ssi, size_t ssi_len,
				const u8 *peer_addr)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_notify_nan_receive(wpa_s, id, peer_instance_id, peer_addr,
				ssi, ssi_len);
}


#ifdef CONFIG_P2P
static void wpas_nan_process_p2p_usd_elems(void *ctx, const u8 *buf,
					   u16 buf_len, const u8 *peer_addr,
					   unsigned int freq)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_p2p_process_usd_elems(wpa_s, buf, buf_len, peer_addr, freq);
}
#endif /* CONFIG_P2P */


#ifdef CONFIG_PR
static void wpas_nan_process_pr_usd_elems(void *ctx, const u8 *buf, u16 buf_len,
					  const u8 *peer_addr,
					  unsigned int freq)
{
	struct wpa_supplicant *wpa_s = ctx;

	wpas_pr_process_usd_elems(wpa_s, buf, buf_len, peer_addr, freq);
}
#endif /* CONFIG_PR */


int wpas_nan_de_init(struct wpa_supplicant *wpa_s)
{
	struct nan_callbacks cb;
	bool offload = !!(wpa_s->drv_flags2 &
			  WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD);

	os_memset(&cb, 0, sizeof(cb));
	cb.ctx = wpa_s;
	cb.tx = wpas_nan_de_tx;
	cb.listen = wpas_nan_de_listen;
	cb.discovery_result = wpas_nan_de_discovery_result;
	cb.replied = wpas_nan_de_replied;
	cb.publish_terminated = wpas_nan_de_publish_terminated;
	cb.subscribe_terminated = wpas_nan_de_subscribe_terminated;
	cb.offload_cancel_publish = wpas_nan_usd_offload_cancel_publish;
	cb.offload_cancel_subscribe = wpas_nan_usd_offload_cancel_subscribe;
	cb.receive = wpas_nan_de_receive;
#ifdef CONFIG_P2P
	cb.process_p2p_usd_elems = wpas_nan_process_p2p_usd_elems;
#endif /* CONFIG_P2P */
#ifdef CONFIG_PR
	cb.process_pr_usd_elems = wpas_nan_process_pr_usd_elems;
#endif /* CONFIG_PR */

	wpa_s->nan_de = nan_de_init(wpa_s->own_addr, offload, false,
				    wpa_s->max_remain_on_chan, &cb);
	if (!wpa_s->nan_de)
		return -1;
	return 0;
}


void wpas_nan_de_deinit(struct wpa_supplicant *wpa_s)
{
	eloop_cancel_timeout(wpas_nan_usd_remain_on_channel_timeout,
			     wpa_s, ELOOP_ALL_CTX);
	nan_de_deinit(wpa_s->nan_de);
	wpa_s->nan_de = NULL;
}


void wpas_nan_de_rx_sdf(struct wpa_supplicant *wpa_s, const u8 *src,
			const u8 *a3, unsigned int freq,
			const u8 *buf, size_t len, int rssi)
{
	bool store_peer;

	if (!wpa_s->nan_de)
		return;

	store_peer = nan_de_rx_sdf(wpa_s->nan_de, src, a3, freq, buf,
				   len, rssi);

	if (!store_peer)
		return;

#ifdef CONFIG_NAN
	if (!wpas_nan_ready(wpa_s))
		return;

	nan_add_peer(wpa_s->nan, src, buf, len);
#endif /* CONFIG_NAN */
}


void wpas_nan_de_flush(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s->nan_de)
		return;
	nan_de_flush(wpa_s->nan_de);
	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		wpas_drv_nan_flush(wpa_s);
}


int wpas_nan_publish(struct wpa_supplicant *wpa_s, const char *service_name,
		     enum nan_service_protocol_type srv_proto_type,
		     const struct wpabuf *ssi,
		     struct nan_publish_params *params, bool p2p)
{
	int publish_id;
	struct wpabuf *elems = NULL;
	const u8 *addr;

	if (!wpa_s->nan_de)
		return -1;

	if (params->proximity_ranging && !params->solicited) {
		wpa_printf(MSG_INFO,
			   "PR unsolicited publish service discovery not allowed");
		return -1;
	}

	addr = wpa_s->own_addr;

#ifdef CONFIG_NAN
	if (params->sync) {
		if (!(wpa_s->nan_capa.drv_flags &
		      WPA_DRIVER_FLAGS_NAN_SUPPORT_USERSPACE_DE)) {
			wpa_printf(MSG_INFO,
				   "NAN: Cannot advertise sync service, driver does not support user space DE");
			return -1;
		}

		if (!wpas_nan_ready(wpa_s)) {
			wpa_printf(MSG_INFO,
				   "NAN: Synchronized support is not enabled");
			return -1;
		}

		if (p2p) {
			wpa_printf(MSG_INFO,
				   "NAN: Sync discovery is not supported for P2P");
			return -1;
		}

		if (params->proximity_ranging) {
			wpa_printf(MSG_INFO,
				   "NAN: Sync discovery is not supported for PR");
			return -1;
		}
	}
#endif /* CONFIG_NAN */

	if (p2p) {
		elems = wpas_p2p_usd_elems(wpa_s, service_name);
		addr = wpa_s->global->p2p_dev_addr;
	} else if (params->proximity_ranging) {
		elems = wpas_pr_usd_elems(wpa_s);
	}

	if (params->forced_addr) {
		if (!(wpa_s->drv_flags & WPA_DRIVER_FLAGS_MGMT_TX_RANDOM_TA)) {
			wpa_printf(MSG_INFO, "NAN: Random TA not allowed");
			return -1;
		}
		addr = params->forced_addr;
	}

	publish_id = nan_de_publish(wpa_s->nan_de, service_name, srv_proto_type,
				    ssi, elems, params, p2p, addr);
	if (publish_id >= 1 && !params->sync &&
	    (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD) &&
	    wpas_drv_nan_publish(wpa_s, addr, publish_id, service_name,
				 nan_de_get_service_id(wpa_s->nan_de,
						       publish_id),
				 srv_proto_type, ssi, elems, params) < 0) {
		nan_de_cancel_publish(wpa_s->nan_de, publish_id);
		publish_id = -1;
	}
#ifdef CONFIG_AP
	if (publish_id >= 1 && wpa_s->ap_iface && wpa_s->ap_iface->bss[0]) {
		wpa_printf(MSG_DEBUG, "NAN: Linking nan_de for AP interface");
		wpa_s->ap_iface->bss[0]->nan_de = wpa_s->nan_de;
	}
#endif /* CONFIG_AP */

	wpabuf_free(elems);
	return publish_id;
}


void wpas_nan_cancel_publish(struct wpa_supplicant *wpa_s, int publish_id)
{
	if (!wpa_s->nan_de)
		return;
	nan_de_cancel_publish(wpa_s->nan_de, publish_id);
	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		wpas_drv_nan_cancel_publish(wpa_s, publish_id);
}


int wpas_nan_update_publish(struct wpa_supplicant *wpa_s, int publish_id,
			    const struct wpabuf *ssi)
{
	int ret;

	if (!wpa_s->nan_de)
		return -1;
	ret = nan_de_update_publish(wpa_s->nan_de, publish_id, ssi);
	if (ret == 0 && (wpa_s->drv_flags2 &
			 WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD) &&
	    wpas_drv_nan_update_publish(wpa_s, publish_id, ssi) < 0)
		return -1;
	return ret;
}


int wpas_nan_usd_unpause_publish(struct wpa_supplicant *wpa_s, int publish_id,
				 u8 peer_instance_id, const u8 *peer_addr)
{
	if (!wpa_s->nan_de)
		return -1;
	return nan_de_unpause_publish(wpa_s->nan_de, publish_id,
				      peer_instance_id, peer_addr);
}


static int wpas_nan_stop_listen(struct wpa_supplicant *wpa_s, int id)
{
	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		return 0;

	if (nan_de_stop_listen(wpa_s->nan_de, id) < 0)
		return -1;

	if (wpa_s->nan_usd_listen_work) {
		wpa_printf(MSG_DEBUG, "NAN: Stop listen operation");
		wpa_drv_cancel_remain_on_channel(wpa_s);
		wpas_nan_usd_listen_work_done(wpa_s);
	}

	if (wpa_s->nan_usd_tx_work) {
		wpa_printf(MSG_DEBUG, "NAN: Stop TX wait operation");
		offchannel_send_action_done(wpa_s);
		wpas_nan_usd_tx_work_done(wpa_s);
	}

	return 0;
}


int wpas_nan_usd_publish_stop_listen(struct wpa_supplicant *wpa_s,
				     int publish_id)
{
	if (!wpa_s->nan_de)
		return -1;

	wpa_printf(MSG_DEBUG, "NAN: Request to stop listen for publish_id=%d",
		   publish_id);
	return wpas_nan_stop_listen(wpa_s, publish_id);
}


int wpas_nan_subscribe(struct wpa_supplicant *wpa_s,
		       const char *service_name,
		       enum nan_service_protocol_type srv_proto_type,
		       const struct wpabuf *ssi,
		       struct nan_subscribe_params *params, bool p2p)
{
	int subscribe_id;
	struct wpabuf *elems = NULL;
	const u8 *addr;

	if (!wpa_s->nan_de)
		return -1;

	if (params->proximity_ranging && !params->active) {
		wpa_printf(MSG_INFO,
			   "PR passive subscriber service discovery not allowed");
		return -1;
	}

	addr = wpa_s->own_addr;

#ifdef CONFIG_NAN
	if (params->sync) {
		if (!(wpa_s->nan_capa.drv_flags &
		      WPA_DRIVER_FLAGS_NAN_SUPPORT_USERSPACE_DE)) {
			wpa_printf(MSG_INFO,
				   "NAN: Cannot subscribe sync, user space DE is not supported");
			return -1;
		}

		if (!wpas_nan_ready(wpa_s)) {
			wpa_printf(MSG_INFO, "NAN: Not ready (subscribe)");
			return -1;
		}

		if (p2p) {
			wpa_printf(MSG_INFO,
				   "NAN: Sync discovery is not supported for P2P (subscribe)");
			return -1;
		}

		if (params->proximity_ranging) {
			wpa_printf(MSG_INFO,
				   "NAN: Sync discovery is not supported for PR (subscribe)");
			return -1;
		}
	}
#endif /* CONFIG_NAN */

	if (p2p) {
		elems = wpas_p2p_usd_elems(wpa_s, service_name);
		addr = wpa_s->global->p2p_dev_addr;
	} else if (params->proximity_ranging) {
		elems = wpas_pr_usd_elems(wpa_s);
	}

	if (params->forced_addr) {
		if (!(wpa_s->drv_flags & WPA_DRIVER_FLAGS_MGMT_TX_RANDOM_TA)) {
			wpa_printf(MSG_INFO, "NAN: Random TA not allowed");
			return -1;
		}
		addr = params->forced_addr;
	}

	subscribe_id = nan_de_subscribe(wpa_s->nan_de, service_name,
					srv_proto_type, ssi, elems, params,
					p2p, addr);
	if (subscribe_id >= 1 && !params->sync &&
	    (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD) &&
	    wpas_drv_nan_subscribe(wpa_s, addr, subscribe_id, service_name,
				   nan_de_get_service_id(wpa_s->nan_de,
							 subscribe_id),
				   srv_proto_type, ssi, elems, params) < 0) {
		nan_de_cancel_subscribe(wpa_s->nan_de, subscribe_id);
		subscribe_id = -1;
	}
#ifdef CONFIG_AP
	if (subscribe_id >= 1 && wpa_s->ap_iface && wpa_s->ap_iface->bss[0]) {
		wpa_printf(MSG_DEBUG, "NAN: Linking nan_de for AP interface");
		wpa_s->ap_iface->bss[0]->nan_de = wpa_s->nan_de;
	}
#endif /* CONFIG_AP */

	wpabuf_free(elems);
	return subscribe_id;
}


void wpas_nan_cancel_subscribe(struct wpa_supplicant *wpa_s,
			       int subscribe_id)
{
	if (!wpa_s->nan_de)
		return;
	nan_de_cancel_subscribe(wpa_s->nan_de, subscribe_id);
	if (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_NAN_USD_OFFLOAD)
		wpas_drv_nan_cancel_subscribe(wpa_s, subscribe_id);
}


int wpas_nan_usd_subscribe_stop_listen(struct wpa_supplicant *wpa_s,
				       int subscribe_id)
{
	if (!wpa_s->nan_de)
		return -1;

	wpa_printf(MSG_DEBUG, "NAN: Request to stop listen for subscribe_id=%d",
		   subscribe_id);
	return wpas_nan_stop_listen(wpa_s, subscribe_id);
}


int wpas_nan_transmit(struct wpa_supplicant *wpa_s, int handle,
		      const struct wpabuf *ssi, const struct wpabuf *elems,
		      const u8 *peer_addr, u8 req_instance_id)
{
	if (!wpa_s->nan_de)
		return -1;
	return nan_de_transmit(wpa_s->nan_de, handle, ssi, elems, peer_addr,
			       req_instance_id);
}


void wpas_nan_usd_remain_on_channel_cb(struct wpa_supplicant *wpa_s,
				       unsigned int freq, unsigned int duration)
{
	wpas_nan_usd_listen_work_done(wpa_s);

	if (wpa_s->nan_de)
		nan_de_listen_started(wpa_s->nan_de, freq, duration);
}


void wpas_nan_usd_cancel_remain_on_channel_cb(struct wpa_supplicant *wpa_s,
					      unsigned int freq)
{
	if (wpa_s->nan_de)
		nan_de_listen_ended(wpa_s->nan_de, freq);
}


void wpas_nan_usd_tx_wait_expire(struct wpa_supplicant *wpa_s)
{
	wpas_nan_usd_tx_work_done(wpa_s);

	if (wpa_s->nan_de)
		nan_de_tx_wait_ended(wpa_s->nan_de);
}


int * wpas_nan_usd_all_freqs(struct wpa_supplicant *wpa_s)
{
	int i, j;
	int *freqs = NULL;

	if (!wpa_s->hw.modes)
		return NULL;

	for (i = 0; i < wpa_s->hw.num_modes; i++) {
		struct hostapd_hw_modes *mode = &wpa_s->hw.modes[i];

		for (j = 0; j < mode->num_channels; j++) {
			struct hostapd_channel_data *chan = &mode->channels[j];

			/* All 20 MHz channels on 2.4 and 5 GHz band */
			if (chan->freq < 2412 || chan->freq > 5900)
				continue;

			/* that allow frames to be transmitted */
			if (chan->flag & (HOSTAPD_CHAN_DISABLED |
					  HOSTAPD_CHAN_NO_IR |
					  HOSTAPD_CHAN_RADAR))
				continue;

			int_array_add_unique(&freqs, chan->freq);
		}
	}

	return freqs;
}


void wpas_nan_usd_state_change_notif(struct wpa_supplicant *wpa_s)
{
	struct wpa_supplicant *ifs;
	unsigned int n_active = 0;
	struct nan_de_cfg cfg;

	if (!wpa_s->radio)
		return;

	os_memset(&cfg, 0, sizeof(cfg));

	dl_list_for_each(ifs, &wpa_s->radio->ifaces, struct wpa_supplicant,
			 radio_list) {
		if (ifs->wpa_state >= WPA_AUTHENTICATING)
			n_active++;
	}

	wpa_printf(MSG_DEBUG,
		   "NAN: state change notif: n_active=%u, p2p_in_progress=%u",
		   n_active, wpas_p2p_in_progress(wpa_s));

	if (n_active) {
		cfg.n_max = 3;

		if (!wpas_p2p_in_progress(wpa_s)) {
			/* Limit the USD operation on channel to 100 - 300 TUs
			 * to allow more time for other interfaces.
			 */
			cfg.n_min = 1;
		} else {
			/* Limit the USD operation on channel to 200 - 300 TUs
			 * to allow P2P operation to complete.
			 */
			cfg.n_min = 2;
		}

		/* Each 500 ms suspend USD operation for 300 ms */
		cfg.cycle = 500;
		cfg.suspend = 300;
	}

	dl_list_for_each(ifs, &wpa_s->radio->ifaces, struct wpa_supplicant,
			 radio_list) {
		if (ifs->nan_de)
			nan_de_config(ifs->nan_de, &cfg);
	}
}
