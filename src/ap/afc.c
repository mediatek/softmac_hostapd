/*
 * Automated Frequency Coordination
 * Copyright (c) 2024, Lorenzo Bianconi <lorenzo@kernel.org>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include <json-c/json.h>
#include <sys/un.h>
#include <time.h>

#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "common/hw_features_common.h"
#include "hostapd.h"
#include "acs.h"
#include "hw_features.h"
#include "ap_drv_ops.h"

#define HOSTAPD_AFC_RETRY_TIMEOUT	180
#define HOSTAPD_AFC_TIMEOUT		86400 /* 24h */
#define HOSTAPD_AFC_BUFSIZE		8192

static void hostapd_afc_timeout_handler(void *eloop_ctx, void *timeout_ctx);
int hostapd_afc_reset_channels(struct hostapd_iface *iface);

static struct json_object *
hostapd_afc_build_location_request(struct hostapd_iface *iface)
{
	struct json_object *location_obj, *center_obj, *ellipse_obj;
	struct json_object *elevation_obj, *str_obj;
	struct hostapd_config *iconf = iface->conf;
	bool is_ap_indoor = he_reg_is_indoor(iconf->he_6ghz_reg_pwr_type);

	location_obj = json_object_new_object();
	if (!location_obj)
		return NULL;

	if (iconf->afc.location.type != LINEAR_POLYGON) {
		struct afc_linear_polygon *lp =
			&iconf->afc.location.linear_polygon_data[0];

		if (!lp)
			goto error;

		ellipse_obj = json_object_new_object();
		if (!ellipse_obj)
			goto error;

		center_obj = json_object_new_object();
		if (!center_obj)
			goto error;

		json_object_object_add(ellipse_obj, "center", center_obj);

		str_obj = json_object_new_double(lp->longitude);
		if (!str_obj)
			goto error;

		json_object_object_add(center_obj, "longitude", str_obj);
		str_obj = json_object_new_double(lp->latitude);
		if (!str_obj)
			goto error;

		json_object_object_add(center_obj, "latitude", str_obj);
	}

	switch (iconf->afc.location.type) {
	case LINEAR_POLYGON: {
		struct json_object *outer_boundary_obj;
		int i;

		outer_boundary_obj = json_object_new_object();
		if (!outer_boundary_obj)
			goto error;

		json_object_object_add(location_obj, "linearPolygon",
				       outer_boundary_obj);
		ellipse_obj = json_object_new_array();
		if (!ellipse_obj)
			goto error;

		json_object_object_add(outer_boundary_obj, "outerBoundary",
				       ellipse_obj);
		for (i = 0;
		     i < iconf->afc.location.n_linear_polygon_data; i++) {
			struct afc_linear_polygon *lp =
				&iconf->afc.location.linear_polygon_data[i];

			center_obj = json_object_new_object();
			if (!center_obj)
				goto error;

			json_object_array_add(ellipse_obj, center_obj);
			str_obj = json_object_new_double(lp->longitude);
			if (!str_obj)
				goto error;

			json_object_object_add(center_obj, "longitude",
					       str_obj);
			str_obj = json_object_new_double(lp->latitude);
			if (!str_obj)
				goto error;

			json_object_object_add(center_obj, "latitude",
					       str_obj);
		}
		break;
	}
	case RADIAL_POLYGON: {
		struct json_object *outer_boundary_obj;
		int i;

		json_object_object_add(location_obj, "radialPolygon",
				       ellipse_obj);

		outer_boundary_obj = json_object_new_array();
		if (!outer_boundary_obj)
			goto error;

		json_object_object_add(ellipse_obj, "outerBoundary",
				       outer_boundary_obj);
		for (i = 0;
		     i < iconf->afc.location.n_radial_polygon_data; i++) {
			struct afc_radial_polygon *rp =
				&iconf->afc.location.radial_polygon_data[i];
			struct json_object *angle_obj;

			angle_obj = json_object_new_object();
			if (!angle_obj)
				goto error;

			json_object_array_add(outer_boundary_obj, angle_obj);

			str_obj = json_object_new_double(rp->angle);
			if (!str_obj)
				goto error;

			json_object_object_add(angle_obj, "angle", str_obj);
			str_obj = json_object_new_double(rp->length);
			if (!str_obj)
				goto error;

			json_object_object_add(angle_obj, "length", str_obj);
		}
		break;
	}
	case ELLIPSE:
	default:
		json_object_object_add(location_obj, "ellipse", ellipse_obj);

		str_obj = json_object_new_int(iconf->afc.location.major_axis);
		if (!str_obj)
			goto error;

		json_object_object_add(ellipse_obj, "majorAxis", str_obj);
		str_obj = json_object_new_int(iconf->afc.location.minor_axis);
		if (!str_obj)
			goto error;

		json_object_object_add(ellipse_obj, "minorAxis", str_obj);
		str_obj = json_object_new_int(iconf->afc.location.orientation);
		if (!str_obj)
			goto error;

		json_object_object_add(ellipse_obj, "orientation", str_obj);
		break;
	}

	elevation_obj = json_object_new_object();
	if (!elevation_obj)
		goto error;

	json_object_object_add(location_obj, "elevation",
			       elevation_obj);
	str_obj = json_object_new_double(iconf->afc.location.height);
	if (!str_obj)
		goto error;

	json_object_object_add(elevation_obj, "height", str_obj);
	if (iconf->afc.location.height_type) {
		str_obj = json_object_new_string(iconf->afc.location.height_type);
		if (!str_obj)
			goto error;

		json_object_object_add(elevation_obj, "heightType", str_obj);
	}

	str_obj = json_object_new_int(iconf->afc.location.vertical_tolerance);
	if (!str_obj)
		goto error;

	json_object_object_add(elevation_obj, "verticalUncertainty",
			       str_obj);
	str_obj = json_object_new_int(is_ap_indoor);
	if (!str_obj)
		goto error;

	json_object_object_add(location_obj, "indoorDeployment", str_obj);

	return location_obj;

error:
	json_object_put(location_obj);
	return NULL;
}


static struct json_object * hostapd_afc_get_opclass_chan_list(u8 op_class)
{
	struct json_object *chan_list_obj, *str_obj;
	const struct oper_class_map *oper_class;
	int chan_offset, chan;

	oper_class = get_oper_class(NULL, op_class);
	if (!oper_class)
		return NULL;

	chan_list_obj = json_object_new_array();
	if (!chan_list_obj)
		return NULL;

	switch (op_class) {
	case 132: /*  40MHz */
		chan_offset = 2;
		break;
	case 133: /*  80MHz */
		chan_offset = 6;
		break;
	case 134: /* 160MHz */
		chan_offset = 14;
		break;
	case 137: /* 320MHz */
		chan_offset = 30;
		break;
	default:
		chan_offset = 0;
		break;
	}

	for (chan = oper_class->min_chan; chan <= oper_class->max_chan;
	     chan += oper_class->inc) {
		if (chan + chan_offset > oper_class->max_chan)
			break;
		str_obj = json_object_new_int(chan + chan_offset);
		if (!str_obj) {
			json_object_put(chan_list_obj);
			return NULL;
		}

		json_object_array_add(chan_list_obj, str_obj);
		if (op_class == 137) { /* 320MHz */
			int c = chan + chan_offset + oper_class->inc / 2;

			if (c > oper_class->max_chan)
				break;

			str_obj = json_object_new_int(c);
			if (!str_obj) {
				json_object_put(chan_list_obj);
				return NULL;
			}

			json_object_array_add(chan_list_obj, str_obj);
		}
	}

	return chan_list_obj;
}


static struct json_object *
hostapd_afc_build_req_chan_list(struct hostapd_iface *iface)
{
	struct json_object *op_class_list_obj, *str_obj;
	struct hostapd_config *iconf = iface->conf;
	int i;

	op_class_list_obj = json_object_new_array();
	if (!op_class_list_obj)
		return NULL;

	for (i = 0; i < iconf->afc.n_op_class; i++) {
		struct json_object *op_class_obj, *chan_list_obj;
		u8 op_class = iconf->afc.op_class[i];

		if (!is_6ghz_op_class(op_class))
			continue;

		op_class_obj = json_object_new_object();
		if (!op_class_obj)
			goto error;

		json_object_array_add(op_class_list_obj, op_class_obj);
		str_obj = json_object_new_int(op_class);
		if (!str_obj)
			goto error;

		json_object_object_add(op_class_obj, "globalOperatingClass",
				       str_obj);

		chan_list_obj = hostapd_afc_get_opclass_chan_list(op_class);
		if (!chan_list_obj)
			goto error;

		json_object_object_add(op_class_obj, "channelCfi",
				       chan_list_obj);
	}

	return op_class_list_obj;

error:
	json_object_put(op_class_list_obj);
	return NULL;
}


static struct json_object *
hostapd_afc_build_request(struct hostapd_iface *iface)
{
	struct json_object *l1_obj, *l2_obj, *la1_obj, *la2_obj;
	struct json_object *s2_obj, *str_obj, *location_obj;
	struct hostapd_config *iconf = iface->conf;
	struct json_object *op_class_list_obj;
	int i;

	l1_obj = json_object_new_object();
	if (!l1_obj)
		return NULL;

	if (iconf->afc.request.version) {
		str_obj = json_object_new_string(iconf->afc.request.version);
		if (!str_obj)
			goto error;

		json_object_object_add(l1_obj, "version", str_obj);
	}

	la1_obj = json_object_new_array();
	if (!la1_obj)
		goto error;

	json_object_object_add(l1_obj, "availableSpectrumInquiryRequests",
			       la1_obj);
	l2_obj = json_object_new_object();
	if (!l2_obj)
		goto error;

	json_object_array_add(la1_obj, l2_obj);
	if (iconf->afc.request.id) {
		str_obj = json_object_new_string(iconf->afc.request.id);
		if (!str_obj)
			goto error;

		json_object_object_add(l2_obj, "requestId", str_obj);
	}

	s2_obj = json_object_new_object();
	if (!s2_obj)
		goto error;

	json_object_object_add(l2_obj, "deviceDescriptor", s2_obj);
	if (iconf->afc.request.sn) {
		str_obj = json_object_new_string(iconf->afc.request.sn);
		if (!str_obj)
			goto error;

		json_object_object_add(s2_obj, "serialNumber", str_obj);
	}

	la2_obj = json_object_new_array();
	if (!la2_obj)
		goto error;

	json_object_object_add(s2_obj, "certificationId", la2_obj);
	for (i = 0; i < iconf->afc.n_cert_ids; i++) {
		struct json_object *obj;

		obj = json_object_new_object();
		if (!obj)
			goto error;

		json_object_array_add(la2_obj, obj);
		str_obj =
			json_object_new_string(iconf->afc.cert_ids[i].rulset);
		if (!str_obj)
			goto error;

		json_object_object_add(obj, "rulesetId", str_obj);
		str_obj = json_object_new_string(iconf->afc.cert_ids[i].id);
		if (!str_obj)
			goto error;

		json_object_object_add(obj, "id", str_obj);
	}

	location_obj = hostapd_afc_build_location_request(iface);
	if (!location_obj)
		goto error;

	json_object_object_add(l2_obj, "location", location_obj);
	str_obj = json_object_new_int(iconf->afc.min_power);
	if (!str_obj)
		goto error;

	json_object_object_add(l2_obj, "minDesiredPower", str_obj);

	if (iconf->afc.n_freq_range) {
		struct json_object *freq_obj;

		freq_obj = json_object_new_array();
		if (!freq_obj)
			goto error;

		json_object_object_add(l2_obj, "inquiredFrequencyRange",
				       freq_obj);
		for (i = 0; i < iconf->afc.n_freq_range; i++) {
			struct afc_freq_range *fr = &iconf->afc.freq_range[i];
			struct json_object *obj;

			obj = json_object_new_object();
			if (!obj)
				goto error;

			json_object_array_add(freq_obj, obj);
			str_obj = json_object_new_int(fr->low_freq);
			if (!str_obj)
				goto error;

			json_object_object_add(obj, "lowFrequency", str_obj);
			str_obj = json_object_new_int(fr->high_freq);
			if (!str_obj)
				goto error;

			json_object_object_add(obj, "highFrequency", str_obj);
		}
	}

	op_class_list_obj = hostapd_afc_build_req_chan_list(iface);
	if (!op_class_list_obj)
		goto error;

	json_object_object_add(l2_obj, "inquiredChannels", op_class_list_obj);

	wpa_printf(MSG_DEBUG, "Pending AFC request: %s",
		   json_object_get_string(l1_obj));

	return l1_obj;

error:
	json_object_put(l1_obj);

	return NULL;
}


static int
hostad_afc_parse_available_freq_info(struct hostapd_iface *iface,
				     struct json_object *reply_elem_obj)
{
	struct afc_freq_range_elem *f = NULL;
	struct json_object *obj;
	int i, count = 0;

	if (!json_object_object_get_ex(reply_elem_obj,
				       "availableFrequencyInfo", &obj))
		return 0;

	for (i = 0; i < json_object_array_length(obj); i++) {
		struct json_object *range_elem_obj, *freq_range_obj;
		struct json_object *high_freq_obj, *low_freq_obj;
		struct json_object *max_psd_obj;

		range_elem_obj = json_object_array_get_idx(obj, i);
		if (!range_elem_obj)
			continue;

		if (!json_object_object_get_ex(range_elem_obj,
					       "frequencyRange",
					       &freq_range_obj))
			continue;

		if (!json_object_object_get_ex(freq_range_obj,
					       "lowFrequency",
					       &low_freq_obj))
			continue;

		if (!json_object_object_get_ex(freq_range_obj,
					       "highFrequency",
					       &high_freq_obj))
			continue;

		if (!json_object_object_get_ex(range_elem_obj, "maxPsd",
					       &max_psd_obj) &&
		    !json_object_object_get_ex(range_elem_obj, "maxPSD",
					       &max_psd_obj))
			continue;

		f = os_realloc_array(f, count + 1, sizeof(*f));
		if (!f)
			return -ENOMEM;

		f[count].low_freq = json_object_get_int(low_freq_obj);
		f[count].high_freq = json_object_get_int(high_freq_obj);
		f[count++].max_psd = json_object_get_int(max_psd_obj);
	}
	iface->afc.freq_range = f;
	iface->afc.num_freq_range = count;

	return 0;
}


static int hostad_afc_update_chan_info(struct afc_chan_info_elem **chan_list,
				       int *chan_list_size, u8 op_class,
				       int center_chan, int power)
{
	int op_class_pwr_index, num_low_subchan, ch, count = *chan_list_size;
	struct afc_chan_info_elem *c = *chan_list;

	switch (op_class) {
	case 132: /*  40MHz */
		op_class_pwr_index = 1;
		num_low_subchan = 2;
		break;
	case 133: /*  80MHz */
		op_class_pwr_index = 2;
		num_low_subchan = 6;
		break;
	case 134: /* 160MHz */
		op_class_pwr_index = 3;
		num_low_subchan = 14;
		break;
	case 137: /* 320MHz */
		op_class_pwr_index = 4;
		num_low_subchan = 30;
		break;
	default:
		op_class_pwr_index = 0;
		num_low_subchan = 0;
		break;
	}

	for (ch = center_chan - num_low_subchan;
	     ch <= center_chan + num_low_subchan; ch += 4) {
		int i;

		for (i = 0; i < count; i++) {
			if (c[i].chan == ch)
				break;
		}

		if (i == count) {
			c = os_realloc_array(c, count + 1, sizeof(*c));
			if (!c)
				return -ENOMEM;

			c[count].chan = ch;
			for (int j = 0; j < 5; j++)
				c[count].power[j] = AFC_INVALID_POWER;
			count++;
		}
		c[i].power[op_class_pwr_index] = power;
	}

	*chan_list_size = count;
	*chan_list = c;

	return 0;
}


static int
hostad_afc_parse_available_chan_info(struct hostapd_iface *iface,
				     struct json_object *reply_elem_obj)
{
	struct afc_chan_info_elem *c = NULL;
	struct json_object *obj;
	int i, count = 0;

	if (!json_object_object_get_ex(reply_elem_obj,
				       "availableChannelInfo", &obj))
		return 0;

	for (i = 0; i < json_object_array_length(obj); i++) {
		struct json_object *range_elem_obj, *op_class_obj;
		struct json_object *chan_cfi_obj, *max_eirp_obj;
		int ch, op_class;

		range_elem_obj = json_object_array_get_idx(obj, i);
		if (!range_elem_obj)
			continue;

		if (!json_object_object_get_ex(range_elem_obj,
					       "globalOperatingClass",
					       &op_class_obj))
			continue;

		if (!json_object_object_get_ex(range_elem_obj, "maxEirp",
					       &max_eirp_obj))
			continue;

		if (!json_object_object_get_ex(range_elem_obj, "channelCfi",
					       &chan_cfi_obj))
			continue;

		op_class = json_object_get_int(op_class_obj);
		for (ch = 0;
		     ch < json_object_array_length(chan_cfi_obj); ch++) {
			struct json_object *pwr_obj;
			struct json_object *ch_obj;
			int channel, power;

			ch_obj = json_object_array_get_idx(chan_cfi_obj, ch);
			if (!ch_obj)
				continue;

			pwr_obj = json_object_array_get_idx(max_eirp_obj, ch);
			if (!pwr_obj)
				continue;

			channel = json_object_get_int(ch_obj);
			power = json_object_get_int(pwr_obj);

			hostad_afc_update_chan_info(&c, &count, op_class,
						    channel, power);
		}
		iface->afc.chan_info_list = c;
		iface->afc.num_chan_info = count;
	}

	return 0;
}


static int hostad_afc_get_timeout(struct json_object *obj)
{
	time_t t, now;
	struct tm tm;

	if (sscanf(json_object_get_string(obj), "%d-%d-%dT%d:%d:%dZ",
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
		   &tm.tm_min, &tm.tm_sec) <= 0)
		return HOSTAPD_AFC_TIMEOUT;

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	t = mktime(&tm);
	time(&now);

	return now > t ? HOSTAPD_AFC_RETRY_TIMEOUT : (t - now) * 80 / 100;
}


static int hostapd_afc_parse_reply(struct hostapd_iface *iface, char *reply)
{
	struct json_object *payload_obj, *reply_obj, *version_obj;
	struct hostapd_config *iconf = iface->conf;
	int i, request_timeout = -1, ret = -EINVAL;

	wpa_printf(MSG_DEBUG, "Received AFC reply: %s", reply);
	payload_obj = json_tokener_parse(reply);
	if (!payload_obj) {
		wpa_printf(MSG_ERROR, "Failed to parse AFC reply payload");
		return -EINVAL;
	}

	if (!json_object_object_get_ex(payload_obj, "version", &version_obj)) {
		wpa_printf(MSG_ERROR, "Missing version in AFC reply");
		return -EINVAL;
	}

	if (iconf->afc.request.version &&
	    os_strcmp(iconf->afc.request.version,
		      json_object_get_string(version_obj))) {
		wpa_printf(MSG_ERROR, "Mismatch in AFC reply version");
		return -EINVAL;
	}

	if (!json_object_object_get_ex(payload_obj,
				       "availableSpectrumInquiryResponses",
				       &reply_obj)) {
		wpa_printf(MSG_ERROR,
			   "Missing availableSpectrumInquiry in AFC reply");
		return -EINVAL;
	}

	for (i = 0; i < json_object_array_length(reply_obj); i++) {
		struct json_object *reply_elem_obj, *obj, *status_obj;
		int j, status = -EINVAL;

		reply_elem_obj = json_object_array_get_idx(reply_obj, i);
		if (!reply_elem_obj) {
			wpa_printf(MSG_DEBUG,
				   "Failed to get reply element at index %d",
				   i);
			continue;
		}

		if (!json_object_object_get_ex(reply_elem_obj, "requestId",
					       &obj)) {
			wpa_printf(MSG_DEBUG,
				   "Missing requestId in reply element %d", i);
			continue;
		}

		if (iconf->afc.request.id &&
		    os_strcmp(iconf->afc.request.id,
			      json_object_get_string(obj))) {
			wpa_printf(MSG_DEBUG,
				   "RequestId mismatch in reply element %d",
				   i);
			continue;
		}

		if (!json_object_object_get_ex(reply_elem_obj, "rulesetId",
					       &obj)) {
			wpa_printf(MSG_DEBUG,
				   "Missing rulesetId in reply element %d", i);
			continue;
		}

		for (j = 0; j < iconf->afc.n_cert_ids; j++) {
			if (!os_strcmp(iconf->afc.cert_ids[j].rulset,
				       json_object_get_string(obj)))
				break;
		}

		if (j == iconf->afc.n_cert_ids) {
			wpa_printf(MSG_DEBUG,
				   "RulesetId mismatch in reply element %d",
				   i);
			continue;
		}

		if (!json_object_object_get_ex(reply_elem_obj, "response",
					       &obj)) {
			wpa_printf(MSG_DEBUG,
				   "Missing response field in reply element %d",
				   i);
			continue;
		}

		if (json_object_object_get_ex(obj, "shortDescription",
					      &status_obj))
			wpa_printf(MSG_DEBUG, "AFC reply element %d: %s",
				   i, json_object_get_string(status_obj));

		if (json_object_object_get_ex(obj, "responseCode",
					      &status_obj))
			status = json_object_get_int(status_obj);

		if (status < 0) {
			wpa_printf(MSG_DEBUG,
				   "Reply element %d invalid responseCode: %d",
				   i, status);
			continue;
		}

		if (hostad_afc_parse_available_freq_info(iface,
							 reply_elem_obj) ||
		    hostad_afc_parse_available_chan_info(iface,
							 reply_elem_obj))
			continue;

		if (json_object_object_get_ex(reply_elem_obj,
					      "availabilityExpireTime",
					      &obj)) {
			int timeout = hostad_afc_get_timeout(obj);

			if (request_timeout < 0 || timeout < request_timeout)
				request_timeout = timeout;
		}

		ret = status;
	}

	iface->afc.data_valid = true;
	iface->afc.timeout = request_timeout;
	if (iface->afc.timeout < 0)
		iface->afc.timeout = HOSTAPD_AFC_RETRY_TIMEOUT;
	else if (iface->afc.timeout > iconf->afc.max_timeout &&
		 iconf->afc.max_timeout >= HOSTAPD_AFC_RETRY_TIMEOUT)
		iface->afc.timeout = iconf->afc.max_timeout;

	return ret;
}


static int hostapd_afc_send_receive(struct hostapd_iface *iface)
{
	struct hostapd_config *iconf = iface->conf;
	json_object *request_obj = NULL;
	struct timeval sock_timeout = {
		.tv_sec = 10,
	};
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
#ifdef __FreeBSD__
		.sun_len = sizeof(addr),
#endif /* __FreeBSD__ */
	};
	const char *request;
	char *buf = NULL;
	int sockfd, ret;
	fd_set read_set;

	if (iface->afc.data_valid) {
		/* AFC data already downloaded from the server */
		return 0;
	}

	iface->afc.timeout = HOSTAPD_AFC_RETRY_TIMEOUT;
	if (!iconf->afc.socket) {
		wpa_printf(MSG_ERROR, "Missing AFC socket string");
		return -EINVAL;
	}

	if (os_strlen(iconf->afc.socket) >= sizeof(addr.sun_path)) {
		wpa_printf(MSG_ERROR, "Malformed AFC socket string %s",
			   iconf->afc.socket);
		return -EINVAL;
	}

	os_strlcpy(addr.sun_path, iconf->afc.socket, sizeof(addr.sun_path));
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		wpa_printf(MSG_ERROR, "Failed creating AFC socket");
		return sockfd;
	}

	if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		wpa_printf(MSG_ERROR, "Failed connecting AFC socket");
		ret = -EIO;
		goto close_sock;
	}

	request_obj = hostapd_afc_build_request(iface);
	if (!request_obj) {
		ret = -ENOMEM;
		goto close_sock;
	}

	request = json_object_to_json_string(request_obj);
	if (send(sockfd, request, strlen(request), 0) < 0) {
		wpa_printf(MSG_ERROR, "Failed sending AFC request");
		ret = -EIO;
		goto close_sock;
	}

	FD_ZERO(&read_set);
	FD_SET(sockfd, &read_set);
	if (select(sockfd + 1, &read_set, NULL, NULL, &sock_timeout) < 0) {
		wpa_printf(MSG_ERROR, "Select failed on AFC socket");
		ret = -errno;
		goto close_sock;
	}

	if (!FD_ISSET(sockfd, &read_set)) {
		ret = -EIO;
		goto close_sock;
	}

	buf = os_zalloc(HOSTAPD_AFC_BUFSIZE);
	if (!buf) {
		ret = -ENOMEM;
		goto close_sock;
	}

	ret = recv(sockfd, buf, HOSTAPD_AFC_BUFSIZE - 1, 0);
	if (!ret)
		ret = -EIO;
	if (ret < 0)
		goto close_sock;

	ret = hostapd_afc_parse_reply(iface, buf);
	if (ret)
		wpa_printf(MSG_ERROR, "Failed parsing AFC reply: %d", ret);
close_sock:
	os_free(buf);
	json_object_put(request_obj);
	close(sockfd);

	return ret;
}


static bool hostapd_afc_has_usable_chans(struct hostapd_iface *iface)
{
	const struct oper_class_map *oper_class;
	int ch, chan_num;

	oper_class = get_oper_class(NULL, iface->conf->op_class);
	if (!oper_class)
		return false;

	switch (iface->conf->op_class) {
	case 132: /*  40MHz */
		chan_num = 2;
		break;
	case 133: /*  80MHz */
		chan_num = 4;
		break;
	case 134: /* 160MHz */
		chan_num = 8;
		break;
	case 137: /* 320MHz */
		chan_num = 16;
		break;
	default:
		chan_num = 1;
		break;
	}

	for (ch = oper_class->min_chan; ch <= oper_class->max_chan;
	     ch += oper_class->inc) {
		struct hostapd_hw_modes *mode = iface->current_mode;
		struct hostapd_channel_data *chan = NULL;
		int i;

		for (i = 0; i < chan_num; i++) {
			chan = hw_get_channel_chan(mode, ch + i * 4, NULL);

			if (!chan || chan->flag & HOSTAPD_CHAN_DISABLED)
				break;
		}

		if (i == chan_num)
			return true;

		if (iface->conf->op_class != 137 || ch < 33 || ch > 193)
			continue;

		/* 320-2 */
		for (i = 0; i < chan_num; i++) {
			chan = hw_get_channel_chan(mode, ch + i * 4 - 32,
						   NULL);

			if (!chan || chan->flag & HOSTAPD_CHAN_DISABLED)
				break;
		}

		if (i == chan_num)
			return true;
	}

	return false;
}


int hostapd_afc_handle_request(struct hostapd_iface *iface)
{
	struct hostapd_config *iconf = iface->conf;
	bool lpi_mode;
	int ret;
	int afc_status = AFC_CONTINUE;

	lpi_mode = he_reg_is_indoor(iconf->he_6ghz_reg_pwr_type);
	if (lpi_mode && !he_reg_is_sp(iconf->he_6ghz_reg_pwr_type)) {
		iface->afc.lpi_mode = true;
		return 1;
	}

	if (strncmp(iconf->country, "US", 2) != 0 &&
	    strncmp(iconf->country, "CA", 2) != 0)
		return 1;

	/* AFC is required just for standard power AP */
	if (!he_reg_is_sp(iconf->he_6ghz_reg_pwr_type))
		return 1;

	if (!is_6ghz_op_class(iconf->op_class) || !is_6ghz_freq(iface->freq))
		return 1;

	if (iface->state == HAPD_IFACE_ACS)
		return 1;

	ret = hostapd_afc_send_receive(iface);
	if (ret < 0) {
		afc_status = lpi_mode ? AFC_LPI : AFC_DISABLE;
		goto resched;
	}

	hostap_afc_disable_channels(iface);
	if (!hostapd_afc_has_usable_chans(iface)) {
		afc_status = lpi_mode ? AFC_LPI : AFC_DISABLE;
		goto resched;
	}

	ret = hostapd_is_usable_chans(iface);
	if (ret != 1) {
		/* Trigger an ACS freq scan */
		afc_status = AFC_RESTART_IFACE;
		iconf->channel = 0;
		iface->freq = 0;
		hostapd_set_and_check_bw320_offset(iface->conf, 0);

		if (!ret && acs_init(iface) != HOSTAPD_CHAN_ACS) {
			wpa_printf(MSG_ERROR, "Could not start ACS");
			afc_status = AFC_DISABLE;
			ret = -EINVAL;
		}
	} else
		ret = 1;

resched:
	switch(afc_status) {
	case AFC_LPI:
		ret = hostapd_afc_reset_channels(iface);
		if (ret) {
			ret = 0;
			break;
		}
		iface->afc.lpi_mode = true;
		ret = 1;
		break;
	/* Disable and restart iface would be finished in hostapd setup flow. */
	case AFC_DISABLE:
		if (ret == -EIO)
			ret = 0;
		break;
	case AFC_RESTART_IFACE:
	case AFC_CONTINUE:
		break;
	default:
		break;
	}

	eloop_cancel_timeout(hostapd_afc_timeout_handler, iface, NULL);
	eloop_register_timeout(iface->afc.timeout, 0,
			       hostapd_afc_timeout_handler, iface, NULL);

	return ret;
}


static void hostapd_afc_delete_data_from_server(struct hostapd_iface *iface)
{
	os_free(iface->afc.chan_info_list);
	os_free(iface->afc.freq_range);

	iface->afc.num_freq_range = 0;
	iface->afc.num_chan_info = 0;

	iface->afc.chan_info_list = NULL;
	iface->afc.freq_range = NULL;

	iface->afc.data_valid = false;
}


static void hostapd_afc_timeout_handler(void *eloop_ctx, void *timeout_ctx)
{
	struct hostapd_iface *iface = eloop_ctx;
	bool lpi_mode;
	int afc_status = AFC_CONTINUE, ret;

	lpi_mode = he_reg_is_indoor(iface->conf->he_6ghz_reg_pwr_type);
	iface->afc.lpi_mode = false;

	hostapd_afc_delete_data_from_server(iface);
	if (iface->state != HAPD_IFACE_ENABLED) {
		afc_status = AFC_RESTART_IFACE;
		/* Hostapd is not fully enabled yet, toggle the interface */
		goto resched;
	}


	if (hostapd_afc_send_receive(iface) < 0 ||
	    hostapd_afc_reset_channels(iface)) {
		afc_status = lpi_mode ? AFC_LPI : AFC_DISABLE;
		goto resched;
	}

	hostap_afc_disable_channels(iface);
	if (hostapd_is_usable_chans(iface) == 1)
		goto resched;

	ret = hostapd_afc_has_usable_chans(iface);
	if (ret) {
		/* Trigger an ACS freq scan */
		afc_status = AFC_RESTART_IFACE;
		iface->conf->channel = 0;
		iface->freq = 0;
		hostapd_set_and_check_bw320_offset(iface->conf, 0);
	} else
		afc_status = lpi_mode ? AFC_LPI : AFC_DISABLE;

resched:
	switch(afc_status) {
	case AFC_DISABLE:
		hostapd_drv_txpower_ctrl(iface->bss[0]);
		hostapd_disable_iface(iface);
		break;
	case AFC_RESTART_IFACE:
		hostapd_disable_iface(iface);
		hostapd_enable_iface(iface);
		break;
	case AFC_LPI:
		if (hostapd_afc_reset_channels(iface)) {
			hostapd_disable_iface(iface);
			break;
		}
		iface->afc.lpi_mode = true;
		fallthrough;
	case AFC_CONTINUE:
		hostapd_drv_txpower_ctrl(iface->bss[0]);
		break;
	default:
		break;
	}
	eloop_register_timeout(iface->afc.timeout, 0,
			       hostapd_afc_timeout_handler, iface, NULL);
}


void hostapd_afc_stop(struct hostapd_iface *iface)
{
	eloop_cancel_timeout(hostapd_afc_timeout_handler, iface, NULL);
}


void hostap_afc_disable_channels(struct hostapd_iface *iface)
{
	struct hostapd_hw_modes *mode = NULL;
	int i;

	for (i = 0; i < iface->num_hw_features; i++) {
		mode = &iface->hw_features[i];
		if (mode->mode == HOSTAPD_MODE_IEEE80211A &&
		    mode->is_6ghz)
			break;
	}

	if (i == iface->num_hw_features)
		return;

	if (!he_reg_is_sp(iface->conf->he_6ghz_reg_pwr_type))
		return;

	if (!iface->afc.data_valid)
		return;

	for (i = 0; i < mode->num_channels; i++) {
		struct hostapd_channel_data *chan = &mode->channels[i];
		int j;

		if (!is_6ghz_freq(chan->freq))
			continue;

		for (j = 0; j < iface->afc.num_freq_range; j++) {
			if (chan->freq >= iface->afc.freq_range[j].low_freq &&
			    chan->freq <= iface->afc.freq_range[j].high_freq)
				break;
		}

		if (j != iface->afc.num_freq_range)
			continue;

		for (j = 0; j < iface->afc.num_chan_info; j++) {
			if (chan->chan == iface->afc.chan_info_list[j].chan)
				break;
		}

		if (j != iface->afc.num_chan_info)
			continue;

		chan->flag |= HOSTAPD_CHAN_DISABLED;
		wpa_printf(MSG_MSGDUMP,
			   "Disabling freq=%d MHz (not allowed by AFC)",
			   chan->freq);
	}
}


int hostapd_afc_reset_channels(struct hostapd_iface *iface)
{
	int ret;

	ret = hostapd_get_hw_features(iface);
	if (ret) {
		wpa_printf(MSG_ERROR, "Failed to reset channel flags");
		return ret;
	}

	ret = hostapd_set_current_hw_info(iface, iface->freq);
	if (ret)
		wpa_printf(MSG_ERROR, "Failed to set current hardware info");

	return ret;
}


int hostap_afc_get_chan_max_eirp_power(struct hostapd_iface *iface, bool psd,
				       int *power)
{
	int i;

	if (!he_reg_is_sp(iface->conf->he_6ghz_reg_pwr_type))
		return -EINVAL;

	if (!iface->afc.data_valid)
		return -EINVAL;

	if (psd) {
		for (i = 0; i < iface->afc.num_freq_range; i++) {
			struct afc_freq_range_elem *f;

			f = &iface->afc.freq_range[i];
			if (iface->freq >= f->low_freq &&
			    iface->freq <= f->high_freq) {
				*power = 2 * f->max_psd;
				return 0;
			}
		}
	} else {
		for (i = 0; i < iface->afc.num_chan_info; i++) {
			struct afc_chan_info_elem *c;

			c = &iface->afc.chan_info_list[i];
			if (c->chan == iface->conf->channel) {
				int j;

				*power = 0;
				for (j = 0; j < 5; j++) {
					if (c->power[j] != AFC_INVALID_POWER)
						*power = MAX(*power, 2 * c->power[j]);
				}

				return 0;
			}
		}
	}
	return -EINVAL;
}

void hostapd_afc_init_power_table(s8 ***power_table)
{
	int table_idx, bw;
	s8 *chan_power_list;

	/* init power table */
	for (table_idx = 0; table_idx < MAX_CHANNEL_NUM_6G; table_idx++) {
		chan_power_list = (*power_table)[table_idx];
		for (bw = 0; bw < afc_power_table_num; bw++)
			chan_power_list[bw] = AFC_INVALID_POWER;
	}
}

void hostapd_afc_parse_bw320(int table_idx, s8 *power_list)
{
	if (table_idx > 7)
		power_list[afc_power_bw320_2] =
			power_list[afc_power_bw320_1];
	if (table_idx > 47)
		power_list[afc_power_bw320_1] = AFC_INVALID_POWER;
}

int hostapd_afc_parse_psd_to_dbm(struct hostapd_iface *iface, s8 ***power_table)
{
	int i, freq, channel, bw, table_idx, target_power;
	s8 *chan_power_list;

	for (i = 0; i < iface->afc.num_freq_range; i++) {
		struct afc_freq_range_elem *freq_range = &iface->afc.freq_range[i];

		freq = freq_range->low_freq + 10;
		channel = hostapd_hw_get_channel(iface->bss[0], freq);
		if (channel == 0)
			continue;

		table_idx = channel / 4;

		if (table_idx >= MAX_CHANNEL_NUM_6G)
			return -EINVAL;

		chan_power_list = (*power_table)[table_idx];
		for (bw = afc_power_bw20; bw < afc_power_bw320_2; bw++) {
			target_power = freq_range->max_psd * 2 + PSD_TO_DBM_OFFSET +
				bw * DOUBLE_BW_POWER;
			target_power = MIN(AFC_MAXIMUM_POWER, target_power);
			chan_power_list[bw] = MIN(chan_power_list[bw],
						  target_power);
		}
		hostapd_afc_parse_bw320(table_idx, chan_power_list);
	}
	return 0;
}

int hostapd_afc_parse_eirp_to_dbm(struct hostapd_iface *iface, s8 ***power_table)
{
	int i, bw, table_idx, target_power;
	s8 *chan_power_list;

	for (i = 0; i < iface->afc.num_chan_info; i++) {
		struct afc_chan_info_elem *chan_info = &iface->afc.chan_info_list[i];

		table_idx = chan_info->chan / 4;

		if (table_idx >= MAX_CHANNEL_NUM_6G)
			return -EINVAL;

		chan_power_list = (*power_table)[table_idx];
		for (bw = afc_power_bw20; bw <= afc_power_bw320_1; bw++) {
			if (chan_info->power[bw] == AFC_INVALID_POWER)
				continue;
			target_power = MIN(AFC_MAXIMUM_POWER, chan_info->power[bw] * 2);
			chan_power_list[bw] = MIN(chan_power_list[bw], target_power);
		}
		hostapd_afc_parse_bw320(table_idx, chan_power_list);
	}
	return 0;
}

int afc_get_ru_be_offset(int bw, int *target_bw, int *offset)
{
	switch (bw) {
	case afc_power_ru26:
		*target_bw = afc_power_bw20;
		*offset = RU26_OFFSET_20MHZ;
		break;
	case afc_power_ru52:
		*target_bw = afc_power_bw20;
		*offset = RU52_OFFSET_20MHZ;
		break;
	case afc_power_ru78:
		*target_bw = afc_power_bw20;
		*offset = RU78_OFFSET_20MHZ;
		break;
	case afc_power_ru106:
		*target_bw = afc_power_bw20;
		*offset = RU106_OFFSET_20MHZ;
		break;
	case afc_power_ru132:
		*target_bw = afc_power_bw20;
		*offset = RU132_OFFSET_20MHZ;
		break;
	case afc_power_ru726:
		*target_bw = afc_power_bw80;
		*offset = RU726_OFFSET_80MHZ;
		break;
	case afc_power_ru1480:
		*target_bw = afc_power_bw160;
		*offset = RU1480_OFFSET_160MHZ;
		break;
	case afc_power_ru1772:
		*target_bw = afc_power_bw160;
		*offset = RU1772_OFFSET_160MHZ;
		break;
	case afc_power_ru2476:
		*target_bw = afc_power_bw320_1;
		*offset = RU2476_OFFSET_320MHZ;
		break;
	case afc_power_ru2988:
		*target_bw = afc_power_bw320_1;
		*offset = RU2988_OFFSET_320MHZ;
		break;
	case afc_power_ru3472:
		*target_bw = afc_power_bw320_1;
		*offset = RU3472_OFFSET_320MHZ;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int hostapd_afc_fill_wide_bandwidth_power(s8 ***power_table)
{
	int table_idx, bw;
	s8 *chan_power_list;

	for (table_idx = 0; table_idx < MAX_CHANNEL_NUM_6G; table_idx++) {
		int target_power, ru26_power;

		if ((*power_table)[table_idx][afc_power_bw20] == AFC_INVALID_POWER)
			continue;

		chan_power_list = (*power_table)[table_idx];

		/* Check wide bandwidth power minimum or valid. */
		for (bw = afc_power_bw40; bw <= afc_power_bw320_2; bw++) {
			int bw_ch_num, first_ch, last_ch;

			target_power = AFC_INVALID_POWER;
			switch (bw) {
			case afc_power_bw40:
				bw_ch_num = 2;
				break;
			case afc_power_bw80:
				bw_ch_num = 4;
				break;
			case afc_power_bw160:
				bw_ch_num = 8;
				break;
			case afc_power_bw320_1:
			case afc_power_bw320_2:
				bw_ch_num = 16;
				break;
			default:
				continue;
			}
			if ((bw == afc_power_bw320_1 && table_idx > 47) ||
			    (bw == afc_power_bw320_2 && table_idx < 8))
				continue;

			if (bw == afc_power_bw320_2)
				first_ch = table_idx - (table_idx + 8) % bw_ch_num;
			else
				first_ch = table_idx - table_idx % bw_ch_num;
			last_ch = first_ch + bw_ch_num;
			for (int ch = first_ch; ch < last_ch; ch++) {
				if ((*power_table)[ch][bw] == AFC_INVALID_POWER) {
					target_power = AFC_INVALID_POWER;
					break;
				}
				target_power = MIN((*power_table)[ch][bw], target_power);
			}
			chan_power_list[bw] = target_power;
		}

		/* Update remain ru */
		for (bw = afc_power_ru26; bw < afc_power_table_num; bw++) {
			int target_bw, offset;

			if (afc_get_ru_be_offset(bw, &target_bw, &offset))
				return -EINVAL;

			if (target_bw == afc_power_bw320_1 &&
			    chan_power_list[target_bw] == AFC_INVALID_POWER)
			    target_bw++;

			if (chan_power_list[target_bw] == AFC_INVALID_POWER) {
				chan_power_list[bw] = AFC_INVALID_POWER;
				continue;
			}

			target_power = chan_power_list[target_bw] - offset;
			chan_power_list[bw] = target_power;
		}
	}
	return 0;
}


int hostapd_afc_translate_table(struct hostapd_iface *iface,
				s8 ***power_table)
{
	int i, ret;

	if (!iface->afc.data_valid)
		return -EINVAL;

	/* afc response that there is no avialible bandwidth to use */
	if (!hostapd_afc_has_usable_chans(iface))
		return 0;

	*power_table = (s8**)os_zalloc(MAX_CHANNEL_NUM_6G * sizeof(s8*));

	if (!(*power_table))
		return -ENOMEM;

	for (i = 0; i < MAX_CHANNEL_NUM_6G; i++) {
		(*power_table)[i] = (s8*)os_zalloc(afc_power_table_num * sizeof(s8));
		if (!(*power_table)[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	hostapd_afc_init_power_table(power_table);

	ret = hostapd_afc_parse_psd_to_dbm(iface, power_table);
	if (ret)
		goto out;

	ret = hostapd_afc_parse_eirp_to_dbm(iface, power_table);
	if (ret)
		goto out;

	ret = hostapd_afc_fill_wide_bandwidth_power(power_table);
	if (ret)
		goto out;

out:
	return ret;
}
