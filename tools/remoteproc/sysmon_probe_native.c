// SPDX-License-Identifier: GPL-2.0
/*
 * sysmon_probe_native - minimal AP-side SSCTL (QMI service 43) client.
 *
 * Rationale: on Apollo/SDX55, the modem registers SSCTL at [43:0x1002]@[2:63]
 * about ~28 s into boot, then panics ~15 s later with ERRFATAL. The kernel
 * qcom_sysmon driver would normally open this connection, but it is tied to
 * remoteproc subsystems; the external ESOC-managed SDX55 has no sysmon client.
 *
 * This daemon:
 *   1. opens an AF_QIPCRTR socket,
 *   2. sends NEW_LOOKUP for QMI service 43 (SSCTL),
 *   3. on NEW_SERVER for [43:*]@[node:port] sends a QMI GET_VERSION_INFO
 *      request (txn 1, msg 0x0020) to that endpoint,
 *   4. holds the socket open indefinitely, draining further QRTR ctrl traffic
 *      and any QMI replies/indications.
 *
 * If this single connection prevents the 15 s post-mission crash, SSCTL is
 * confirmed as the missing piece.
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

#define SSCTL_SVC_ID		43
#define QMI_GET_VERSION_REQ	0x0020
#define SDX55_SSCTL_INSTANCE	0x1002	/* version 2, instance 0x10 */

#define LOG_PREFIX "sysmon-probe: "

static volatile sig_atomic_t g_stop;
static uint32_t g_match_instance = SDX55_SSCTL_INSTANCE;
static uint32_t g_local_node;

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

static int send_ctrl(int fd, uint32_t local_node,
		     const struct qrtr_ctrl_pkt *pkt)
{
	struct sockaddr_qrtr to = {
		.sq_family = AF_QIPCRTR,
		.sq_node   = local_node,
		.sq_port   = QRTR_PORT_CTRL,
	};
	ssize_t n;

	n = sendto(fd, pkt, sizeof(*pkt), 0,
		   (struct sockaddr *)&to, sizeof(to));
	if (n < 0) {
		logmsg("sendto(ctrl node=%u) failed: %s",
		       local_node, strerror(errno));
		return -1;
	}
	return 0;
}

static int send_new_lookup(int fd, uint32_t local_node, uint32_t svc)
{
	struct qrtr_ctrl_pkt pkt = {0};

	pkt.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
	pkt.server.service  = htole32(svc);
	pkt.server.instance = 0;	/* wildcard */
	return send_ctrl(fd, local_node, &pkt);
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
		.type    = 0,		/* QMI_REQUEST */
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
	logmsg("sent QMI GET_VERSION_INFO_REQ to SSCTL @[%u:%u]", node, port);
	return 0;
}

static void handle_ctrl_pkt(int fd, const void *buf, size_t len,
			    int *sscttl_open)
{
	const struct qrtr_ctrl_pkt *pkt = buf;
	uint32_t cmd, svc, inst, node, port;

	if (len < sizeof(*pkt)) {
		logmsg("short ctrl pkt: %zu B", len);
		return;
	}

	cmd  = le32toh(pkt->cmd);
	if (cmd == QRTR_TYPE_NEW_SERVER || cmd == QRTR_TYPE_DEL_SERVER) {
		svc  = le32toh(pkt->server.service);
		inst = le32toh(pkt->server.instance);
		node = le32toh(pkt->server.node);
		port = le32toh(pkt->server.port);

		if (svc == 0 && node == 0 && port == 0)
			return;	/* end-of-list marker */

		logmsg("ctrl %s [%u:0x%x]@[%u:%u]",
		       cmd == QRTR_TYPE_NEW_SERVER ? "NEW_SERVER" : "DEL_SERVER",
		       svc, inst, node, port);

		if (cmd == QRTR_TYPE_NEW_SERVER && svc == SSCTL_SVC_ID) {
			if (node == g_local_node) {
				logmsg("  (skip: local node %u, owned by kernel qcom_sysmon)",
				       node);
			} else if (inst != g_match_instance) {
				logmsg("  (skip: instance 0x%x != target 0x%x)",
				       inst, g_match_instance);
			} else if (*sscttl_open) {
				logmsg("  (skip: already opened SSCTL)");
			} else {
				logmsg("found target SSCTL — opening QMI connection");
				if (send_qmi_get_version(fd, node, port) == 0)
					*sscttl_open = 1;
			}
		}
	} else {
		logmsg("ctrl cmd=%u len=%zu", cmd, len);
	}
}

static void handle_data_pkt(const void *buf, size_t len,
			    const struct sockaddr_qrtr *from)
{
	const uint8_t *p = buf;
	uint16_t txn, msg, mlen;

	if (len >= 7) {
		txn  = p[1] | (p[2] << 8);
		msg  = p[3] | (p[4] << 8);
		mlen = p[5] | (p[6] << 8);
		logmsg("QMI from [%u:%u] type=%u txn=%u msg=0x%04x mlen=%u total=%zu",
		       from->sq_node, from->sq_port, p[0], txn, msg, mlen, len);
	} else {
		logmsg("short data from [%u:%u]: %zu B",
		       from->sq_node, from->sq_port, len);
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_qrtr self;
	socklen_t slen = sizeof(self);
	int fd;
	int ssctl_open = 0;
	uint8_t buf[4096];

	if (argc > 1) {
		g_match_instance = (uint32_t)strtoul(argv[1], NULL, 0);
		fprintf(stderr, LOG_PREFIX "target instance overridden: 0x%x\n",
			g_match_instance);
	}

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
	self.sq_port = 0;	/* kernel auto-assigns */
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
	logmsg("bound node=%u port=%u (target instance=0x%x)",
	       self.sq_node, self.sq_port, g_match_instance);
	g_local_node = self.sq_node;

	if (send_new_lookup(fd, self.sq_node, SSCTL_SVC_ID) < 0) {
		close(fd);
		return 1;
	}
	logmsg("NEW_LOOKUP for service %u (SSCTL) sent", SSCTL_SVC_ID);

	while (!g_stop) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN,
		};
		struct sockaddr_qrtr from;
		socklen_t flen = sizeof(from);
		ssize_t n;
		int rc;

		rc = poll(&pfd, 1, 500);
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
			handle_ctrl_pkt(fd, buf, n, &ssctl_open);
		else
			handle_data_pkt(buf, n, &from);
	}

	logmsg("exiting");
	close(fd);
	return 0;
}
