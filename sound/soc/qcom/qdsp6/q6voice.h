/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_H
#define _Q6_VOICE_H

enum q6voice_path_type {
	/*
	 * Android maps VSID 0x11c05000 (VoiceMMode1) to passive-session
	 * index 6 and uses that index as the APR source port. The session name
	 * alone is not sufficient: using source port 0 creates the legacy Voice
	 * passive session, which the modem's VoiceMMode1 state machine never
	 * joins even though every ADSP command succeeds.
	 */
	Q6VOICE_PATH_VOICEMMODE1 = 6,
	Q6VOICE_PATH_VOICE	= Q6VOICE_PATH_VOICEMMODE1,
	/* TODO: Q6VOICE_PATH_VOIP	= 1, */
	/* TODO: Q6VOICE_PATH_VOLTE	= 2, */
	/* TODO: Q6VOICE_PATH_VOICE2	= 3, */
	/* TODO: Q6VOICE_PATH_QCHAT	= 4, */
	/* TODO: Q6VOICE_PATH_VOWLAN	= 5, */
	/* TODO: Q6VOICE_PATH_VOICEMMODE2	= 7, */
	Q6VOICE_PATH_COUNT	= 8,
};

struct q6voice;

struct q6voice *q6voice_create(struct device *dev);
int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture);
void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index);

#endif /*_Q6_VOICE_H */
