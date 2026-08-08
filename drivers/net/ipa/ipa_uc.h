/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2024 Linaro Ltd.
 */
#ifndef _IPA_UC_H_
#define _IPA_UC_H_

struct ipa;

/**
 * ipa_uc_interrupt_handler() - Handler for microcontroller IPA interrupts
 * @ipa:	IPA pointer
 * @irq_id:	IPA interrupt ID
 */
void ipa_uc_interrupt_handler(struct ipa *ipa, enum ipa_irq_id irq_id);

/**
 * ipa_uc_config() - Configure the IPA microcontroller subsystem
 * @ipa:	IPA pointer
 */
void ipa_uc_config(struct ipa *ipa);

/**
 * ipa_uc_deconfig() - Inverse of ipa_uc_config()
 * @ipa:	IPA pointer
 */
void ipa_uc_deconfig(struct ipa *ipa);

/**
 * ipa_uc_power() - Take a proxy power reference for the microcontroller
 * @ipa:	IPA pointer
 *
 * The first time the modem boots, it loads firmware for and starts the
 * IPA-resident microcontroller.  The microcontroller signals that it
 * has completed its initialization by sending an INIT_COMPLETED response
 * message to the AP.  The AP must ensure the IPA is powered until
 * it receives this message, and to do so we take a "proxy" clock
 * reference on its behalf here.  Once we receive the INIT_COMPLETED
 * message (in ipa_uc_response_hdlr()) we drop this power reference.
 */
void ipa_uc_power(struct ipa *ipa);

/**
 * ipa_uc_mhi_remote_info() - map an MHI doorbell into an IPA uC mailbox
 * @ipa:		IPA pointer
 * @remote_addr:	MHI channel/event doorbell physical address
 * @mailbox:		IPA uC mailbox number
 *
 * Return: 0 on success, negative errno on failure.
 */
int ipa_uc_mhi_remote_info(struct ipa *ipa, u32 remote_addr, u32 mailbox);

/**
 * ipa_uc_panic_notifier()
 * @ipa:	IPA pointer
 *
 * Notifier function called when the system crashes, to inform the
 * microcontroller of the event.
 */
void ipa_uc_panic_notifier(struct ipa *ipa);

#endif /* _IPA_UC_H_ */
