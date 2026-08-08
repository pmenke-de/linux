// SPDX-License-Identifier: GPL-2.0
/*
 * dms_nas_probe - minimal AP-side QMI client for DMS (svc 1) and NAS (svc 2).
 *
 * Rationale: the SDX55 modem's MCFG framework blocks at boot waiting for
 * "data ready" rcevt signal from the Data service. The Data service only
 * initializes when the modem's protocol stack starts — which requires the
 * AP to connect to DMS (Device Management Service) and/or NAS (Network
 * Access Stratum) as the Radio Interface Layer does on Android.
 *
 * Without these connections, MCFG waits 15 s then triggers ERRFATAL.
 *
 * This daemon:
 *   1. sends NEW_LOOKUP for DMS (svc 1) and NAS (svc 2),
 *   2. on NEW_SERVER, sends GET_VERSION_INFO to each,
 *   3. holds connections open indefinitely.
 *
 * If this prevents the crash, the missing protocol-stack trigger is confirmed.
 */

#include <endian.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/qrtr.h>

#define DMS_SVC_ID		1
#define NAS_SVC_ID		2
#define QMI_GET_VERSION_REQ	0x0020

#define LOG_PREFIX "dms-nas-probe: "

struct service {
	uint32_t svc_id;
	const char *name;
	int opened;
	uint32_t node;
	uint32_t port;
};

static volatile sig_atomic_t g_stop;
static uint32_t g_local_node;
static struct service g_services[] = {
	{ DMS_SVC_ID, "DMS", 0, 0, 0 },
	{ NAS_SVC_ID, "NAS", 0, 0, 0 },
};

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%5ld.%03ld] " LOG_PREFIX,
		(long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static int send_ctrl(int fd, const struct qrtr_ctrl_pkt *pkt)
{
	struct sockaddr_qrtr to = {
		.sq_family = AF_QIPCRTR,
		.sq_node   = g_local_node,
		.sq_port   = QRTR_PORT_CTRL,
	};
	ssize_t n;

	n = sendto(fd, pkt, sizeof(*pkt), 0,
		   (struct sockaddr *)&to, sizeof(to));
	if (n < 0) {
		logmsg("sendto(ctrl) failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int send_new_lookup(int fd, uint32_t svc)
{
	struct qrtr_ctrl_pkt pkt = {0};

	pkt.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
	pkt.server.service  = htole32(svc);
	pkt.server.instance = 0;
	return send_ctrl(fd, &pkt);
}

static int send_qmi_get_version(int fd, uint32_t node, uint32_t port)
{
	struct sockaddr_qrtr to = {
		.sq_family = AF_QIPCRTR,
		.sq_node   = node,
		.sq_port   = port,
	};
	struct {
		uint8_t  type;
		uint16_t txn_id;
		uint16_t msg_id;
		uint16_t msg_len;
	} __attribute__((packed)) hdr = {
		.type    = 0,
		.txn_id  = htole16(1),
		.msg_id  = htole16(QMI_GET_VERSION_REQ),
		.msg_len = 0,
	};
	ssize_t n;

	n = sendto(fd, &hdr, sizeof(hdr), 0,
		   (struct sockaddr *)&to, sizeof(to));
	if (n < 0) {
		logmsg("sendto(QMI ver_req %u:%u) failed: %s",
		       node, port, strerror(errno));
		return -1;
	}
	return 0;
}

static struct service *find_service(uint32_t svc_id)
{
	for (size_t i = 0; i < sizeof(g_services) / sizeof(g_services[0]); i++) {
		if (g_services[i].svc_id == svc_id)
			return &g_services[i];
	}
	return NULL;
}

static void handle_ctrl_pkt(int fd, const void *buf, size_t len)
{
	const struct qrtr_ctrl_pkt *pkt = buf;
	uint32_t cmd, svc, inst, node, port;

	if (len < sizeof(*pkt))
		return;

	cmd = le32toh(pkt->cmd);
	if (cmd != QRTR_TYPE_NEW_SERVER && cmd != QRTR_TYPE_DEL_SERVER)
		return;

	svc  = le32toh(pkt->server.service);
	inst = le32toh(pkt->server.instance);
	node = le32toh(pkt->server.node);
	port = le32toh(pkt->server.port);

	if (svc == 0 && node == 0 && port == 0)
		return;

	struct service *s = find_service(svc);
	if (!s)
		return;

	logmsg("ctrl %s [%u:0x%x]@[%u:%u] (%s)",
	       cmd == QRTR_TYPE_NEW_SERVER ? "NEW_SERVER" : "DEL_SERVER",
	       svc, inst, node, port, s->name);

	if (cmd == QRTR_TYPE_NEW_SERVER && !s->opened) {
		if (send_qmi_get_version(fd, node, port) == 0) {
			s->opened = 1;
			s->node = node;
			s->port = port;
			logmsg("sent GET_VERSION_INFO to %s @[%u:%u]",
			       s->name, node, port);
		}
	}
}

static void handle_data_pkt(const void *buf, size_t len,
			    const struct sockaddr_qrtr *from)
{
	const uint8_t *p = buf;
	uint16_t txn, msg, mlen;
	const char *type_str = "?";

	if (len >= 7) {
		txn  = p[1] | (p[2] << 8);
		msg  = p[3] | (p[4] << 8);
		mlen = p[5] | (p[6] << 8);

		switch (p[0]) {
		case 0: type_str = "REQ"; break;
		case 1: type_str = "RESP"; break;
		case 2: type_str = "IND"; break;
		}

		logmsg("QMI %s from [%u:%u] txn=%u msg=0x%04x mlen=%u",
		       type_str, from->sq_node, from->sq_port, txn, msg, mlen);
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_qrtr self;
	socklen_t slen = sizeof(self);
	int fd;
	(void)argc;
	(void)argv;

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket(AF_QIPCRTR)");
		return 1;
	}

	if (getsockname(fd, (struct sockaddr *)&self, &slen) < 0) {
		perror("getsockname");
		close(fd);
		return 1;
	}
	self.sq_port = 0;
	if (bind(fd, (struct sockaddr *)&self, sizeof(self)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}
	slen = sizeof(self);
	if (getsockname(fd, (struct sockaddr *)&self, &slen) < 0) {
		perror("getsockname(post-bind)");
		close(fd);
		return 1;
	}
	g_local_node = self.sq_node;
	logmsg("bound node=%u port=%u", self.sq_node, self.sq_port);

	for (size_t i = 0; i < sizeof(g_services) / sizeof(g_services[0]); i++) {
		if (send_new_lookup(fd, g_services[i].svc_id) < 0) {
			close(fd);
			return 1;
		}
		logmsg("NEW_LOOKUP for service %u (%s) sent",
		       g_services[i].svc_id, g_services[i].name);
	}

	while (!g_stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct sockaddr_qrtr from;
		socklen_t flen = sizeof(from);
		uint8_t buf[4096];
		ssize_t n;

		int rc = poll(&pfd, 1, 500);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll: %s", strerror(errno));
			break;
		}
		if (rc == 0)
			continue;

		n = recvfrom(fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&from, &flen);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			logmsg("recvfrom: %s", strerror(errno));
			break;
		}

		if (from.sq_port == QRTR_PORT_CTRL)
			handle_ctrl_pkt(fd, buf, n);
		else
			handle_data_pkt(buf, n, &from);

		/* Check if all services opened */
		int all_open = 1;
		for (size_t i = 0; i < sizeof(g_services) / sizeof(g_services[0]); i++) {
			if (!g_services[i].opened) {
				all_open = 0;
				break;
			}
		}
		if (all_open) {
			logmsg("all services opened, holding connections open");
			/* Keep running to hold connections; re-check periodically */
		}
	}

	logmsg("exiting");
	close(fd);
	return 0;
}
