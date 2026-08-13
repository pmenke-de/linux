/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_MVM_H
#define _Q6_MVM_H

#include "q6voice.h"

struct q6voice_session;

struct q6voice_session *q6mvm_session_create(enum q6voice_path_type path);

int q6mvm_attach(struct q6voice_session *mvm, struct q6voice_session *cvp,
		 bool state);
int q6mvm_attach_stream(struct q6voice_session *mvm, struct q6voice_session *cvs,
			bool state);
int q6mvm_set_tty_mode(struct q6voice_session *mvm, u32 mode);
int q6mvm_start(struct q6voice_session *mvm, bool state);
int q6mvm_get_cvd_version(char *version, size_t len);
int q6mvm_set_mailbox_memory(u64 adsp_iova, u64 pcie_iova, u32 size);
int q6mvm_map_memory(struct q6voice_session *mvm, dma_addr_t table_addr,
		     u32 table_size, u32 *handle);
int q6mvm_unmap_memory(struct q6voice_session *mvm, u32 handle);

#endif /*_Q6_MVM_H */
