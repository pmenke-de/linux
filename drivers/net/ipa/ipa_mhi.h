/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (C) 2026 Linaro Ltd. */
#ifndef _IPA_MHI_H_
#define _IPA_MHI_H_

struct ipa;
struct ipa_mhi_alloc_channel_req;
struct ipa_mhi_alloc_channel_rsp;
struct ipa_mhi_clk_vote_req;
struct ipa_mhi_clk_vote_rsp;
struct qmi_handle;
struct sockaddr_qrtr;

int ipa_mhi_setup(struct ipa *ipa);
void ipa_mhi_teardown(struct ipa *ipa);
int ipa_mhi_modem_ready(struct ipa *ipa);
void ipa_mhi_modem_shutdown(struct ipa *ipa);
void ipa_mhi_modem_server(struct ipa *ipa, struct sockaddr_qrtr *sq);
void ipa_mhi_modem_server_alt(struct ipa *ipa, struct sockaddr_qrtr *sq);
int ipa_mhi_send_ready(struct ipa *ipa);
void ipa_mhi_alloc_channel(struct ipa *ipa,
			   const struct ipa_mhi_alloc_channel_req *req,
			   struct ipa_mhi_alloc_channel_rsp *rsp);
void ipa_mhi_clk_vote(struct ipa *ipa,
		      const struct ipa_mhi_clk_vote_req *req,
		      struct ipa_mhi_clk_vote_rsp *rsp);

#endif /* _IPA_MHI_H_ */
