// SPDX-License-Identifier: GPL-2.0
/*
 * ssctl_notify - hand a subsystem an SSR event over QMI, from userspace.
 *
 * The kernel's sysmon already does this (drivers/remoteproc/qcom_sysmon.c),
 * but it does it once, at boot, and logs nothing on success. When you want to
 * know whether a DSP actually accepted being told that some *other* subsystem
 * came up - and what it says if it did not - there is no way to ask from the
 * outside. This sends the same QMI request by hand and prints the answer.
 *
 *	ssctl_notify -n 5 -p 2 -s esoc0 -e 1
 *
 * Read the answer before believing it. Every SSCTL server on this platform -
 * ADSP, CDSP, SLPI and the modem alike - answers a request from here with
 * result=1 error=2, including the byte-identical message the kernel had
 * already sent and had accepted moments earlier. They are checking who is
 * asking, not what is asked, so a rejection here says nothing about the
 * subsystem name and this cannot be used to find out which names a DSP knows.
 * It is still the only way to see an SSCTL result code at all, since the
 * kernel logs one only when it is bad.
 *
 * Message 0x23 is a notification and nothing more. Do not confuse it with
 * 0x20 (RESTART_REQ), which really does restart the subsystem you send it to.
 *
 * Copyright (c) 2026
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/qrtr.h>

/* musl carries the constant but not always the name. */
#ifndef AF_QIPCRTR
#define AF_QIPCRTR			42
#endif

#define SSCTL_SUBSYS_EVENT_REQ		0x0023
#define SSCTL_SUBSYS_NAME_LENGTH	15

/* QMI service header: flags, transaction, message id, payload length. */
#define QMI_HEADER_LEN			7

#define SSCTL_SSR_EVENT_FORCED		0

static const char *const event_name[] = {
	"before_powerup", "after_powerup", "before_shutdown", "after_shutdown",
};

static bool verbose;

static void hexdump(const char *what, const uint8_t *buf, int len)
{
	int i;

	if (!verbose)
		return;

	printf("  %s (%d bytes):", what, len);
	for (i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	printf("\n");
}

/* QMI TLV: one type byte, a 16-bit length, then the value. */
static uint8_t *put_tlv(uint8_t *p, uint8_t type, const void *val, uint16_t len)
{
	*p++ = type;
	*p++ = len & 0xff;
	*p++ = len >> 8;
	memcpy(p, val, len);

	return p + len;
}

static int build_request(uint8_t *buf, uint16_t txn, const char *name,
			 uint32_t event)
{
	uint8_t name_tlv[1 + SSCTL_SUBSYS_NAME_LENGTH];
	uint32_t forced = SSCTL_SSR_EVENT_FORCED;
	uint8_t *p = buf;
	uint16_t msg_len;
	size_t name_len;

	name_len = strlen(name);
	if (name_len > SSCTL_SUBSYS_NAME_LENGTH) {
		fprintf(stderr, "subsystem name '%s' is longer than %d bytes\n",
			name, SSCTL_SUBSYS_NAME_LENGTH);
		return -1;
	}

	/* Leave room for the header; its length field needs the payload. */
	p = buf + QMI_HEADER_LEN;

	/*
	 * TLV 1 is a counted string: the length is a separate leading byte
	 * rather than the TLV length, so a trailing NUL is neither sent nor
	 * expected.
	 */
	name_tlv[0] = name_len;
	memcpy(name_tlv + 1, name, name_len);
	p = put_tlv(p, 0x01, name_tlv, 1 + name_len);

	p = put_tlv(p, 0x02, &event, sizeof(event));
	p = put_tlv(p, 0x10, &forced, sizeof(forced));

	msg_len = p - buf - QMI_HEADER_LEN;

	buf[0] = 0x00;			/* request */
	buf[1] = txn & 0xff;
	buf[2] = txn >> 8;
	buf[3] = SSCTL_SUBSYS_EVENT_REQ & 0xff;
	buf[4] = SSCTL_SUBSYS_EVENT_REQ >> 8;
	buf[5] = msg_len & 0xff;
	buf[6] = msg_len >> 8;

	return msg_len + QMI_HEADER_LEN;
}

/* Response is a single result TLV: two 16-bit values, result then error. */
static int parse_response(const uint8_t *buf, int len, uint16_t txn)
{
	uint16_t result, error;
	int pos = QMI_HEADER_LEN;

	if (len < QMI_HEADER_LEN) {
		fprintf(stderr, "short response (%d bytes)\n", len);
		return -1;
	}

	if ((buf[1] | buf[2] << 8) != txn) {
		fprintf(stderr, "response for a different transaction\n");
		return -1;
	}

	while (pos + 3 <= len) {
		uint8_t type = buf[pos];
		uint16_t tlen = buf[pos + 1] | buf[pos + 2] << 8;

		if (type == 0x02 && tlen >= 4) {
			result = buf[pos + 3] | buf[pos + 4] << 8;
			error = buf[pos + 5] | buf[pos + 6] << 8;

			printf("  result=%u error=%u -> %s\n", result, error,
			       result == 0 ? "ACCEPTED" : "REJECTED");
			return result == 0 ? 0 : 1;
		}
		pos += 3 + tlen;
	}

	fprintf(stderr, "no result TLV in response\n");
	return -1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -n <node> -p <port> -s <subsys> [-e <event>]\n"
		"\n"
		"  -n  QRTR node of the subsystem to tell (ADSP is 5)\n"
		"  -p  QRTR port of its SSCTL service (usually 2)\n"
		"  -s  name of the subsystem being reported, e.g. esoc0\n"
		"  -e  0 before_powerup, 1 after_powerup (default),\n"
		"      2 before_shutdown, 3 after_shutdown\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR };
	const char *subsys = NULL;
	unsigned int node = 0, port = 0;
	uint32_t event = 1;
	uint8_t buf[256];
	uint16_t txn = 0x4242;
	struct pollfd pfd;
	socklen_t slen;
	int len, fd, ret;
	int opt;

	while ((opt = getopt(argc, argv, "n:p:s:e:vh")) != -1) {
		switch (opt) {
		case 'n': node = strtoul(optarg, NULL, 0); break;
		case 'p': port = strtoul(optarg, NULL, 0); break;
		case 's': subsys = optarg; break;
		case 'e': event = strtoul(optarg, NULL, 0); break;
		case 'v': verbose = true; break;
		default: usage(argv[0]); return 1;
		}
	}

	if (!subsys || !node || !port || event > 3) {
		usage(argv[0]);
		return 1;
	}

	len = build_request(buf, txn, subsys, event);
	if (len < 0)
		return 1;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket(AF_QIPCRTR)");
		return 1;
	}

	sq.sq_node = node;
	sq.sq_port = port;

	printf("telling %u:%u that '%s' is %s\n", node, port, subsys,
	       event_name[event]);

	hexdump("request", buf, len);

	if (sendto(fd, buf, len, 0, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
		perror("sendto");
		close(fd);
		return 1;
	}

	pfd.fd = fd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 5000) <= 0) {
		fprintf(stderr, "  no response within 5s\n");
		close(fd);
		return 1;
	}

	slen = sizeof(sq);
	len = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&sq, &slen);
	if (len < 0) {
		perror("recvfrom");
		close(fd);
		return 1;
	}

	hexdump("response", buf, len);

	ret = parse_response(buf, len, txn);
	close(fd);

	return ret < 0 ? 1 : ret;
}
