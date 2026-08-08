/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */
#ifndef _IPA_QMI_MSG_H_
#define _IPA_QMI_MSG_H_

/* === Only "ipa_qmi" and "ipa_qmi_msg.c" should include this file === */

#include <linux/types.h>

#include <linux/soc/qcom/qmi.h>

/* Request/response/indication QMI message ids used for IPA.  Receiving
 * end issues a response for requests; indications require no response.
 */
#define IPA_QMI_INDICATION_REGISTER	0x20	/* modem -> AP request */
#define IPA_QMI_INIT_DRIVER		0x21	/* AP -> modem request */
#define IPA_QMI_INIT_COMPLETE		0x22	/* AP -> modem indication */
#define IPA_QMI_DRIVER_INIT_COMPLETE	0x35	/* modem -> AP request */
#define IPA_QMI_MHI_CLK_VOTE		0x3b	/* modem -> AP request */
#define IPA_QMI_MHI_READY		0x3c	/* AP -> modem indication */
#define IPA_QMI_MHI_ALLOC_CHANNEL	0x3d	/* modem -> AP request */

/* The maximum size required for message types.  These sizes include
 * the message data, along with type (1 byte) and length (2 byte)
 * information for each field.  The qmi_send_*() interfaces require
 * the message size to be provided.
 */
#define IPA_QMI_INDICATION_REGISTER_REQ_SZ	20	/* -> server handle */
#define IPA_QMI_INDICATION_REGISTER_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_INIT_DRIVER_REQ_SZ		162	/* client handle -> */
#define IPA_QMI_INIT_DRIVER_RSP_SZ		25	/* client handle <- */
#define IPA_QMI_INIT_COMPLETE_IND_SZ		7	/* <- server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ	4	/* -> server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_MHI_CLK_VOTE_REQ_SZ		18	/* -> server handle */
#define IPA_QMI_MHI_CLK_VOTE_RSP_SZ		7	/* <- server handle */
#define IPA_QMI_MHI_READY_IND_SZ		123	/* <- server handle */
#define IPA_QMI_MHI_ALLOC_CHANNEL_REQ_SZ	808	/* -> server handle */
#define IPA_QMI_MHI_ALLOC_CHANNEL_RSP_SZ	23	/* <- server handle */

/* Maximum size of messages we expect the AP to receive (max of above) */
#define IPA_QMI_SERVER_MAX_RCV_SZ		808
#define IPA_QMI_CLIENT_MAX_RCV_SZ		25

#define IPA_QMI_REMOTE_MHI_CHANNELS_MAX		2
#define IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX		2

/* Request message for the IPA_QMI_INDICATION_REGISTER request */
struct ipa_indication_register_req {
	u8 master_driver_init_complete_valid;
	u8 master_driver_init_complete;
	u8 data_usage_quota_reached_valid;
	u8 data_usage_quota_reached;
	u8 ipa_mhi_ready_ind_valid;
	u8 ipa_mhi_ready_ind;
	u8 endpoint_desc_ind_valid;
	u8 endpoint_desc_ind;
	u8 bw_change_ind_valid;
	u8 bw_change_ind;
};

/* The response to a IPA_QMI_INDICATION_REGISTER request consists only of
 * a standard QMI response.
 */
struct ipa_indication_register_rsp {
	struct qmi_response_type_v01 rsp;
};

/* Request message for the IPA_QMI_DRIVER_INIT_COMPLETE request */
struct ipa_driver_init_complete_req {
	u8 status;
};

/* The response to a IPA_QMI_DRIVER_INIT_COMPLETE request consists only
 * of a standard QMI response.
 */
struct ipa_driver_init_complete_rsp {
	struct qmi_response_type_v01 rsp;
};

/* The message for the IPA_QMI_INIT_COMPLETE_IND indication consists
 * only of a standard QMI response.
 */
struct ipa_init_complete_ind {
	struct qmi_response_type_v01 status;
};

/* The AP tells the modem its platform type.  We assume Android. */
enum ipa_platform_type {
	IPA_QMI_PLATFORM_TYPE_INVALID		= 0x0,	/* Invalid */
	IPA_QMI_PLATFORM_TYPE_TN		= 0x1,	/* Data card */
	IPA_QMI_PLATFORM_TYPE_LE		= 0x2,	/* Data router */
	IPA_QMI_PLATFORM_TYPE_MSM_ANDROID	= 0x3,	/* Android MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_WINDOWS	= 0x4,	/* Windows MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_QNX_V01	= 0x5,	/* QNX MSM */
};

/* This defines the start and end offset of a range of memory.  The start
 * value is a byte offset relative to the start of IPA shared memory.  The
 * end value is the last addressable unit *within* the range.  Typically
 * the end value is in units of bytes, however it can also be a maximum
 * array index value.
 */
struct ipa_mem_bounds {
	u32 start;
	u32 end;
};

/* This defines the location and size of an array.  The start value
 * is an offset relative to the start of IPA shared memory.  The
 * size of the array is implied by the number of entries (the entry
 * size is assumed to be known).
 */
struct ipa_mem_array {
	u32 start;
	u32 count;
};

/* This defines the location and size of a range of memory.  The
 * start is an offset relative to the start of IPA shared memory.
 * This differs from the ipa_mem_bounds structure in that the size
 * (in bytes) of the memory region is specified rather than the
 * offset of its last byte.
 */
struct ipa_mem_range {
	u32 start;
	u32 size;
};

/* The message for the IPA_QMI_INIT_DRIVER request contains information
 * from the AP that affects modem initialization.
 */
struct ipa_init_modem_driver_req {
	u8			platform_type_valid;
	u32			platform_type;	/* enum ipa_platform_type */

	/* Modem header table information.  This defines the IPA shared
	 * memory in which the modem may insert header table entries.
	 */
	u8			hdr_tbl_info_valid;
	struct ipa_mem_bounds	hdr_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of non-hashable IPv4 and
	 * IPv6 routing tables.  The start values are byte offsets relative
	 * to the start of IPA shared memory.
	 */
	u8			v4_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_route_tbl_info;
	u8			v6_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_route_tbl_info;

	/* Filter table information.  These define the location of the
	 * non-hashable IPv4 and IPv6 filter tables.  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_filter_tbl_start_valid;
	u32			v4_filter_tbl_start;
	u8			v6_filter_tbl_start_valid;
	u32			v6_filter_tbl_start;

	/* Modem memory information.  This defines the location and
	 * size of memory available for the modem to use.
	 */
	u8			modem_mem_info_valid;
	struct ipa_mem_range	modem_mem_info;

	/* This defines the destination endpoint on the AP to which
	 * the modem driver can send control commands.  Must be less
	 * than ipa_endpoint_max().
	 */
	u8			ctrl_comm_dest_end_pt_valid;
	u32			ctrl_comm_dest_end_pt;

	/* This defines whether the modem should load the microcontroller
	 * or not.  It is unnecessary to reload it if the modem is being
	 * restarted.
	 *
	 * NOTE: this field is named "is_ssr_bootup" elsewhere.
	 */
	u8			skip_uc_load_valid;
	u8			skip_uc_load;

	/* Processing context memory information.  This defines the memory in
	 * which the modem may insert header processing context table entries.
	 */
	u8			hdr_proc_ctx_tbl_info_valid;
	struct ipa_mem_bounds	hdr_proc_ctx_tbl_info;

	/* Compression command memory information.  This defines the memory
	 * in which the modem may insert compression/decompression commands.
	 */
	u8			zip_tbl_info_valid;
	struct ipa_mem_bounds	zip_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of hashable IPv4 and IPv6
	 * routing tables (if supported by hardware).  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_hash_route_tbl_info;
	u8			v6_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_hash_route_tbl_info;

	/* Filter table information.  These define the location and size
	 * of hashable IPv4 and IPv6 filter tables (if supported by hardware).
	 * The start values are byte offsets relative to the start of IPA
	 * shared memory.
	 */
	u8			v4_hash_filter_tbl_start_valid;
	u32			v4_hash_filter_tbl_start;
	u8			v6_hash_filter_tbl_start_valid;
	u32			v6_hash_filter_tbl_start;

	/* Statistics information.  These define the locations of the
	 * first and last statistics sub-regions.  (IPA v4.0 and above)
	 */
	u8			hw_stats_quota_base_addr_valid;
	u32			hw_stats_quota_base_addr;
	u8			hw_stats_quota_size_valid;
	u32			hw_stats_quota_size;
	u8			hw_stats_drop_base_addr_valid;
	u32			hw_stats_drop_base_addr;
	u8			hw_stats_drop_size_valid;
	u32			hw_stats_drop_size;
};

/* The response to a IPA_QMI_INIT_DRIVER request begins with a standard
 * QMI response, but contains other information as well.  Currently we
 * simply wait for the INIT_DRIVER transaction to complete and
 * ignore any other data that might be returned.
 */
struct ipa_init_modem_driver_rsp {
	struct qmi_response_type_v01	rsp;

	/* This defines the destination endpoint on the modem to which
	 * the AP driver can send control commands.  Must be less than
	 * ipa_endpoint_max().
	 */
	u8				ctrl_comm_dest_end_pt_valid;
	u32				ctrl_comm_dest_end_pt;

	/* This defines the default endpoint.  The AP driver is not
	 * required to configure the hardware with this value.  Must
	 * be less than ipa_endpoint_max().
	 */
	u8				default_end_pt_valid;
	u32				default_end_pt;

	/* This defines whether a second handshake is required to complete
	 * initialization.
	 */
	u8				modem_driver_init_pending_valid;
	u8				modem_driver_init_pending;
};

struct ipa_mhi_ch_init_info {
	u8 ch_id;
	u8 er_id;
	u32 ch_doorbell_addr;
	u32 er_doorbell_addr;
	u32 direction_type;
};

struct ipa_mhi_smmu_info {
	u64 iova_ctl_base_addr;
	u64 iova_ctl_size;
	u64 iova_data_base_addr;
	u64 iova_data_size;
};

struct ipa_mhi_ready_ind {
	u32 ch_info_arr_len;
	struct ipa_mhi_ch_init_info ch_info_arr[IPA_QMI_REMOTE_MHI_CHANNELS_MAX];
	u8 smmu_info_valid;
	struct ipa_mhi_smmu_info smmu_info;
};

enum ipa_mhi_brst_mode {
	IPA_MHI_BRST_MODE_DEFAULT = 0,
	IPA_MHI_BRST_MODE_ENABLED = 1,
	IPA_MHI_BRST_MODE_DISABLED = 2,
};

struct ipa_mhi_tr_info {
	u8 ch_id;
	u16 poll_cfg;
	u32 brst_mode_type;
	u64 ring_iova;
	u64 ring_len;
	u64 rp;
	u64 wp;
};

struct ipa_mhi_er_info {
	u8 er_id;
	u32 intmod_cycles;
	u32 intmod_count;
	u32 msi_addr;
	u64 ring_iova;
	u64 ring_len;
	u64 rp;
	u64 wp;
};

struct ipa_mhi_mem_addr_info {
	u64 pa;
	u64 iova;
	u64 size;
};

struct ipa_mhi_alloc_channel_req {
	u32 tr_info_arr_len;
	struct ipa_mhi_tr_info tr_info_arr[IPA_QMI_REMOTE_MHI_CHANNELS_MAX];
	u32 er_info_arr_len;
	struct ipa_mhi_er_info er_info_arr[IPA_QMI_REMOTE_MHI_CHANNELS_MAX];
	u32 ctrl_addr_map_info_len;
	struct ipa_mhi_mem_addr_info
		ctrl_addr_map_info[IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX];
	u32 data_addr_map_info_len;
	struct ipa_mhi_mem_addr_info
		data_addr_map_info[IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX];
};

struct ipa_mhi_ch_alloc_resp {
	u8 ch_id;
	u8 is_success;
};

struct ipa_mhi_alloc_channel_rsp {
	struct qmi_response_type_v01 rsp;
	u8 alloc_resp_arr_valid;
	u32 alloc_resp_arr_len;
	struct ipa_mhi_ch_alloc_resp
		alloc_resp_arr[IPA_QMI_REMOTE_MHI_CHANNELS_MAX];
};

struct ipa_mhi_clk_vote_req {
	u8 mhi_vote;
	u8 tput_value_valid;
	u32 tput_value;
	u8 clk_rate_valid;
	u32 clk_rate;
};

struct ipa_mhi_clk_vote_rsp {
	struct qmi_response_type_v01 rsp;
};

/* Message structure definitions defined in "ipa_qmi_msg.c" */
extern const struct qmi_elem_info ipa_indication_register_req_ei[];
extern const struct qmi_elem_info ipa_indication_register_rsp_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_req_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_rsp_ei[];
extern const struct qmi_elem_info ipa_init_complete_ind_ei[];
extern const struct qmi_elem_info ipa_mem_bounds_ei[];
extern const struct qmi_elem_info ipa_mem_array_ei[];
extern const struct qmi_elem_info ipa_mem_range_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_req_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_rsp_ei[];
extern const struct qmi_elem_info ipa_mhi_ready_ind_ei[];
extern const struct qmi_elem_info ipa_mhi_alloc_channel_req_ei[];
extern const struct qmi_elem_info ipa_mhi_alloc_channel_rsp_ei[];
extern const struct qmi_elem_info ipa_mhi_clk_vote_req_ei[];
extern const struct qmi_elem_info ipa_mhi_clk_vote_rsp_ei[];

#endif /* !_IPA_QMI_MSG_H_ */
