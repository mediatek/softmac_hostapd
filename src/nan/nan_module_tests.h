/*
 * NAN NDP/NDL state machine testing definitions
 * Copyright (C) 2025 Intel Corporation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef NAN_MODULE_TESTS_H
#define NAN_MODULE_TESTS_H

#define NAN_TEST_NAME_MAX     32

/**
 * enum nan_test_ndp_notify_type - NDP notification
 * NAN_TEST_NDP_NOTIFY_INVALID: Invalid
 * NAN_TEST_NDP_NOTIFY_REQUEST: NDP request
 * NAN_TEST_NDP_NOTIFY_RESPONSE: NDP response
 * NAN_TEST_NDP_NOTIFY_CONNECTED: NDP connected
 * NAN_TEST_NDP_NOTIFY_DISCONNECTED: NDP disconnected
 */
enum nan_test_ndp_notify_type {
	NAN_TEST_NDP_NOTIFY_INVALID,
	NAN_TEST_NDP_NOTIFY_REQUEST,
	NAN_TEST_NDP_NOTIFY_RESPONSE,
	NAN_TEST_NDP_NOTIFY_CONNECTED,
	NAN_TEST_NDP_NOTIFY_DISCONNECTED,
};

#define NAN_TEST_MAX_POT_AVAIL 256
#define NAN_MAX_NUM_NDPS       8

/**
 * nan_test_dev_conf - NAN test device configuration
 * @schedule_cb: Callback to configure different schedule handling policies
 *     for a device for initial request and response.
 * @schedule_conf_cb: Callback to configure different schedule handling
 *     policies for a device for confirmation.
 * @get_chans_cb: Callback to configure different channels for potential
 *     availability.
 * @pot_avail: Device potential availability
 * @pot_avail_len: Length of the device potential availability
 * @n_ndps: Number of NDP configurations (to support scenarios that involve
 *      establishing multiple NDPs in a single test case).
 * @ndp_confs: Array of NDP configurations.
 * @accept_request: For publisher device, indicates whether to accept an NDP
 *     request or reject it. For subscriber device, indicates whether to accept
 *     an NDP response or reject it.
 * @term_once_connected: Terminate once connected.
 * @expected_result: Expected NDP establishment result
 * @reason: For publisher device, indicates the reject reason
 */
struct nan_test_dev_conf {
	int (*schedule_cb)(struct nan_schedule *sched);
	int (*schedule_conf_cb)(struct nan_schedule *sched);
	int (*get_chans_cb)(struct nan_channels *chans);

	u8 pot_avail[NAN_TEST_MAX_POT_AVAIL];
	size_t pot_avail_len;

	size_t n_ndps;
	struct ndp_conf {
		bool accept_request;
		bool term_once_connected;
		enum nan_test_ndp_notify_type expected_result;
		u8 reason;
	} ndp_confs[NAN_MAX_NUM_NDPS];
};

/**
 * nan_device - Represents a NAN test device
 * @list: Used for global devices list
 * @global: Pointer to NAN test global data structure
 * @name: Test device name
 * @nmi: Test device NMI
 * @counter: Device counter for NDP various purposes
 * @pot_avail: Device potential availability
 * @pot_avail_len: Length of the device potential availability
 * @nan: Pointer to NAN data structure
 * @conf: NAN test device configuration
 * @n_ndps: Number of NDPs established so far minus one
 * @connected_notify_received: Indicates whether a connected notification
 *     was received
 * @disconnected_notify_received: Indicates whether a disconnected notification
 *     was received
 */
struct nan_device {
	struct dl_list list;
	struct nan_test_global *global;

	u8 name[NAN_TEST_NAME_MAX];
	u8 nmi[ETH_ALEN];
	u32 counter;

	u8 *pot_avail;
	size_t pot_avail_len;

	struct nan_data *nan;
	const struct nan_test_dev_conf *conf;

	size_t n_ndps;

	bool connected_notify_received;
	bool disconnected_notify_received;
};

/**
 * nan_test_case - Single NAN test configuration
 * @name: Test name
 * @pub_conf: Publisher test configuration
 * @sub_conf: Subscriber test configuration
 */
struct nan_test_case {
	const char *name;
	struct nan_test_dev_conf pub_conf;
	struct nan_test_dev_conf sub_conf;
};

const struct nan_test_case * nan_test_case_get_next(void);

#endif /* NAN_MODULE_TESTS_H */
