// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */
#include <linux/stddef.h>

#include <linux/soc/qcom/qmi.h>

#include "ipa_qmi_msg.h"

/* QMI message structure definition for struct ipa_indication_register_req */
const struct qmi_elem_info ipa_indication_register_req_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     master_driver_init_complete_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_indication_register_req,
					   master_driver_init_complete_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     master_driver_init_complete),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_indication_register_req,
					   master_driver_init_complete),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     data_usage_quota_reached_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_indication_register_req,
					   data_usage_quota_reached_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     data_usage_quota_reached),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_indication_register_req,
					   data_usage_quota_reached),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     ipa_mhi_ready_ind_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_indication_register_req,
					   ipa_mhi_ready_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     ipa_mhi_ready_ind),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_indication_register_req,
					   ipa_mhi_ready_ind),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     endpoint_desc_ind_valid),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_indication_register_req,
					   endpoint_desc_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     endpoint_desc_ind),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_indication_register_req,
					   endpoint_desc_ind),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     bw_change_ind_valid),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_indication_register_req,
					   bw_change_ind_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_req,
				     bw_change_ind),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_indication_register_req,
					   bw_change_ind),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_indication_register_rsp */
const struct qmi_elem_info ipa_indication_register_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_indication_register_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_indication_register_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_driver_init_complete_req */
const struct qmi_elem_info ipa_driver_init_complete_req_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_driver_init_complete_req,
				     status),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_driver_init_complete_req,
					   status),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_driver_init_complete_rsp */
const struct qmi_elem_info ipa_driver_init_complete_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_driver_init_complete_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_driver_init_complete_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_complete_ind */
const struct qmi_elem_info ipa_init_complete_ind_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_complete_ind,
				     status),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_init_complete_ind,
					   status),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_bounds */
const struct qmi_elem_info ipa_mem_bounds_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_bounds, start),
		.offset		= offsetof(struct ipa_mem_bounds, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_bounds, end),
		.offset		= offsetof(struct ipa_mem_bounds, end),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_array */
const struct qmi_elem_info ipa_mem_array_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_array, start),
		.offset		= offsetof(struct ipa_mem_array, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_array, count),
		.offset		= offsetof(struct ipa_mem_array, count),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_mem_range */
const struct qmi_elem_info ipa_mem_range_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_range, start),
		.offset		= offsetof(struct ipa_mem_range, start),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_mem_range, size),
		.offset		= offsetof(struct ipa_mem_range, size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_modem_driver_req */
const struct qmi_elem_info ipa_init_modem_driver_req_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     platform_type_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   platform_type_valid),
	},
	{
		.data_type	= QMI_SIGNED_4_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     platform_type),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   platform_type),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_tbl_info_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_tbl_info),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_route_tbl_info_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_route_tbl_info),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_route_tbl_info_valid),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_route_tbl_info),
		.tlv_type	= 0x13,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_filter_tbl_start_valid),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_filter_tbl_start),
		.tlv_type	= 0x14,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_filter_tbl_start_valid),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_filter_tbl_start),
		.tlv_type	= 0x15,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     modem_mem_info_valid),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   modem_mem_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     modem_mem_info),
		.tlv_type	= 0x16,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   modem_mem_info),
		.ei_array	= ipa_mem_range_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     ctrl_comm_dest_end_pt_valid),
		.tlv_type	= 0x17,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   ctrl_comm_dest_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     ctrl_comm_dest_end_pt),
		.tlv_type	= 0x17,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   ctrl_comm_dest_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     skip_uc_load_valid),
		.tlv_type	= 0x18,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   skip_uc_load_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     skip_uc_load),
		.tlv_type	= 0x18,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   skip_uc_load),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_proc_ctx_tbl_info_valid),
		.tlv_type	= 0x19,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_proc_ctx_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hdr_proc_ctx_tbl_info),
		.tlv_type	= 0x19,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hdr_proc_ctx_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     zip_tbl_info_valid),
		.tlv_type	= 0x1a,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   zip_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     zip_tbl_info),
		.tlv_type	= 0x1a,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   zip_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_route_tbl_info_valid),
		.tlv_type	= 0x1b,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_route_tbl_info),
		.tlv_type	= 0x1b,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_route_tbl_info_valid),
		.tlv_type	= 0x1c,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_route_tbl_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_route_tbl_info),
		.tlv_type	= 0x1c,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_route_tbl_info),
		.ei_array	= ipa_mem_bounds_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_filter_tbl_start_valid),
		.tlv_type	= 0x1d,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v4_hash_filter_tbl_start),
		.tlv_type	= 0x1d,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v4_hash_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_filter_tbl_start_valid),
		.tlv_type	= 0x1e,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_filter_tbl_start_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     v6_hash_filter_tbl_start),
		.tlv_type	= 0x1e,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   v6_hash_filter_tbl_start),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_base_addr_valid),
		.tlv_type	= 0x1f,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_base_addr_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_base_addr),
		.tlv_type	= 0x1f,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_base_addr),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_size_valid),
		.tlv_type	= 0x20,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_size_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_quota_size),
		.tlv_type	= 0x20,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_quota_size),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_base_addr_valid),
		.tlv_type	= 0x21,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_base_addr_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_base_addr),
		.tlv_type	= 0x21,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_base_addr),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_size_valid),
		.tlv_type	= 0x22,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_size_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_req,
				     hw_stats_drop_size),
		.tlv_type	= 0x22,
		.offset		= offsetof(struct ipa_init_modem_driver_req,
					   hw_stats_drop_size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

/* QMI message structure definition for struct ipa_init_modem_driver_rsp */
const struct qmi_elem_info ipa_init_modem_driver_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     ctrl_comm_dest_end_pt_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   ctrl_comm_dest_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     ctrl_comm_dest_end_pt),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   ctrl_comm_dest_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     default_end_pt_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   default_end_pt_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     default_end_pt),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   default_end_pt),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     modem_driver_init_pending_valid),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   modem_driver_init_pending_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_init_modem_driver_rsp,
				     modem_driver_init_pending),
		.tlv_type	= 0x12,
		.offset		= offsetof(struct ipa_init_modem_driver_rsp,
					   modem_driver_init_pending),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_ch_init_info_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_init_info,
					       ch_id),
		.offset		= offsetof(struct ipa_mhi_ch_init_info, ch_id),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_init_info,
					       er_id),
		.offset		= offsetof(struct ipa_mhi_ch_init_info, er_id),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_init_info,
					       ch_doorbell_addr),
		.offset		= offsetof(struct ipa_mhi_ch_init_info,
					   ch_doorbell_addr),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_init_info,
					       er_doorbell_addr),
		.offset		= offsetof(struct ipa_mhi_ch_init_info,
					   er_doorbell_addr),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_init_info,
					       direction_type),
		.offset		= offsetof(struct ipa_mhi_ch_init_info,
					   direction_type),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_smmu_info_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_smmu_info,
					       iova_ctl_base_addr),
		.offset		= offsetof(struct ipa_mhi_smmu_info,
					   iova_ctl_base_addr),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_smmu_info,
					       iova_ctl_size),
		.offset		= offsetof(struct ipa_mhi_smmu_info,
					   iova_ctl_size),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_smmu_info,
					       iova_data_base_addr),
		.offset		= offsetof(struct ipa_mhi_smmu_info,
					   iova_data_base_addr),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_smmu_info,
					       iova_data_size),
		.offset		= offsetof(struct ipa_mhi_smmu_info,
					   iova_data_size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

const struct qmi_elem_info ipa_mhi_ready_ind_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_mhi_ready_ind,
					   ch_info_arr_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_CHANNELS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_ready_ind,
					       ch_info_arr[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_mhi_ready_ind,
					   ch_info_arr),
		.ei_array	= ipa_mhi_ch_init_info_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ready_ind,
					       smmu_info_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_ready_ind,
					   smmu_info_valid),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ready_ind,
					       smmu_info),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_ready_ind, smmu_info),
		.ei_array	= ipa_mhi_smmu_info_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_mem_addr_info_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_mem_addr_info, pa),
		.offset		= offsetof(struct ipa_mhi_mem_addr_info, pa),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_mem_addr_info, iova),
		.offset		= offsetof(struct ipa_mhi_mem_addr_info, iova),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_mem_addr_info, size),
		.offset		= offsetof(struct ipa_mhi_mem_addr_info, size),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_tr_info_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, ch_id),
		.offset		= offsetof(struct ipa_mhi_tr_info, ch_id),
	},
	{
		.data_type	= QMI_UNSIGNED_2_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, poll_cfg),
		.offset		= offsetof(struct ipa_mhi_tr_info, poll_cfg),
	},
	{
		.data_type	= QMI_SIGNED_4_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info,
					       brst_mode_type),
		.offset		= offsetof(struct ipa_mhi_tr_info,
					   brst_mode_type),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, ring_iova),
		.offset		= offsetof(struct ipa_mhi_tr_info, ring_iova),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, ring_len),
		.offset		= offsetof(struct ipa_mhi_tr_info, ring_len),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, rp),
		.offset		= offsetof(struct ipa_mhi_tr_info, rp),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_tr_info, wp),
		.offset		= offsetof(struct ipa_mhi_tr_info, wp),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_er_info_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, er_id),
		.offset		= offsetof(struct ipa_mhi_er_info, er_id),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info,
					       intmod_cycles),
		.offset		= offsetof(struct ipa_mhi_er_info, intmod_cycles),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info,
					       intmod_count),
		.offset		= offsetof(struct ipa_mhi_er_info, intmod_count),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, msi_addr),
		.offset		= offsetof(struct ipa_mhi_er_info, msi_addr),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, ring_iova),
		.offset		= offsetof(struct ipa_mhi_er_info, ring_iova),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, ring_len),
		.offset		= offsetof(struct ipa_mhi_er_info, ring_len),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, rp),
		.offset		= offsetof(struct ipa_mhi_er_info, rp),
	},
	{
		.data_type	= QMI_UNSIGNED_8_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_er_info, wp),
		.offset		= offsetof(struct ipa_mhi_er_info, wp),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

const struct qmi_elem_info ipa_mhi_alloc_channel_req_ei[] = {
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   tr_info_arr_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_CHANNELS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_req,
					       tr_info_arr[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   tr_info_arr),
		.ei_array	= ipa_mhi_tr_info_ei,
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   er_info_arr_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_CHANNELS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_req,
					       er_info_arr[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   er_info_arr),
		.ei_array	= ipa_mhi_er_info_ei,
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x03,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   ctrl_addr_map_info_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_req,
					       ctrl_addr_map_info[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x03,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   ctrl_addr_map_info),
		.ei_array	= ipa_mhi_mem_addr_info_ei,
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x04,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   data_addr_map_info_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_MEM_MAPS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_req,
					       data_addr_map_info[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x04,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_req,
					   data_addr_map_info),
		.ei_array	= ipa_mhi_mem_addr_info_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

static const struct qmi_elem_info ipa_mhi_ch_alloc_resp_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_alloc_resp,
					       ch_id),
		.offset		= offsetof(struct ipa_mhi_ch_alloc_resp, ch_id),
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_ch_alloc_resp,
					       is_success),
		.offset		= offsetof(struct ipa_mhi_ch_alloc_resp,
					   is_success),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

const struct qmi_elem_info ipa_mhi_alloc_channel_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_rsp,
					       rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_rsp, rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_rsp,
					       alloc_resp_arr_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_rsp,
					   alloc_resp_arr_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_rsp,
					   alloc_resp_arr_len),
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= IPA_QMI_REMOTE_MHI_CHANNELS_MAX,
		.elem_size	= sizeof_field(struct ipa_mhi_alloc_channel_rsp,
					       alloc_resp_arr[0]),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_alloc_channel_rsp,
					   alloc_resp_arr),
		.ei_array	= ipa_mhi_ch_alloc_resp_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

const struct qmi_elem_info ipa_mhi_clk_vote_req_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_req,
					       mhi_vote),
		.tlv_type	= 0x01,
		.offset		= offsetof(struct ipa_mhi_clk_vote_req,
					   mhi_vote),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_req,
					       tput_value_valid),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_clk_vote_req,
					   tput_value_valid),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_req,
					       tput_value),
		.tlv_type	= 0x10,
		.offset		= offsetof(struct ipa_mhi_clk_vote_req,
					   tput_value),
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_req,
					       clk_rate_valid),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_mhi_clk_vote_req,
					   clk_rate_valid),
	},
	{
		.data_type	= QMI_SIGNED_4_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_req,
					       clk_rate),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_mhi_clk_vote_req,
					   clk_rate),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

const struct qmi_elem_info ipa_mhi_clk_vote_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof_field(struct ipa_mhi_clk_vote_rsp, rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_mhi_clk_vote_rsp, rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};
