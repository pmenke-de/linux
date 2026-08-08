// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal AP-side IPA-QMI proxy for external SDX55 modem (Apollo).
 *
 * Upstream kernel's drivers/net/ipa/ipa_qmi.c registers a QMI client for
 * service 0x31 (IPA_MODEM_SERVICE_SVC_ID = local-Q6 IPA). The external
 * SDX55 advertises a different service: 0x47 (IPA_QMI_SVC_ID). Upstream
 * therefore never claims it, so SDX55 sits waiting for an INIT_DRIVER
 * request that never arrives, then self-triggers ERRFATAL ~15s into
 * MISSION. Downstream covers this with a userspace `ipa_mhi_proxy`
 * daemon. This is a minimal replacement.
 *
 * Protocol (QMI-IPA handshake, mirror of drivers/net/ipa/ipa_qmi.c):
 *   AP    -> SDX55 : INIT_DRIVER           (0x21) request
 *   SDX55 -> AP    : INIT_DRIVER           (0x21) response
 *   SDX55 -> AP    : INDICATION_REGISTER   (0x20) request
 *   AP    -> SDX55 : INDICATION_REGISTER   (0x20) response (SUCCESS)
 *   AP    -> SDX55 : INIT_COMPLETE         (0x22) indication
 *   SDX55 -> AP    : DRIVER_INIT_COMPLETE  (0x35) request
 *   AP    -> SDX55 : DRIVER_INIT_COMPLETE  (0x35) response (SUCCESS)
 *
 * We need to both act as a QMI client (to send INIT_DRIVER and receive
 * its response) and announce a QMI server for svc=0x31 (IPA_HOST_SERVICE)
 * so SDX55 can send INDICATION_REGISTER and DRIVER_INIT_COMPLETE to us.
 *
 * Compiles on-device under musl with only kernel UAPI headers.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

/* QMI header types */
#define QMI_REQUEST     0
#define QMI_RESPONSE    2
#define QMI_INDICATION  4

/* IPA-QMI service IDs (SDX55 side / AP side) */
#define IPA_MODEM_SVC_ID_SDX55		0x47	/* What SDX55 advertises */
#define IPA_HOST_SVC_ID_AP		0x31	/* What AP announces (mirror of local-Q6 value) */

/* IPA-QMI message IDs (match drivers/net/ipa/ipa_qmi_msg.h) */
#define IPA_QMI_INDICATION_REGISTER	0x20	/* modem -> AP request */
#define IPA_QMI_INIT_DRIVER		0x21	/* AP -> modem request */
#define IPA_QMI_INIT_COMPLETE		0x22	/* AP -> modem indication */
#define IPA_QMI_DRIVER_INIT_COMPLETE	0x35	/* modem -> AP request */

/* QMI result codes */
#define QMI_RESULT_SUCCESS_V01		0
#define QMI_RESULT_FAILURE_V01		1
#define QMI_ERR_NONE_V01		0
#define QMI_ERR_NOT_SUPPORTED_V01	94

/* ipa_platform_type */
#define IPA_QMI_PLATFORM_TYPE_LE	2	/* Data router */
#define IPA_QMI_PLATFORM_TYPE_LE_MHI	6	/* Data router over MHI */

struct qmi_header {
	uint8_t  type;
	uint16_t txn_id;
	uint16_t msg_id;
	uint16_t msg_len;
} __attribute__((packed));

struct qrtr_ctrl_pkt_wire {
	uint32_t cmd;
	union {
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
		struct {
			uint32_t node;
			uint32_t port;
		} client;
	};
} __attribute__((packed));

struct config {
	unsigned int target_svc;	/* SDX55 service we're hunting (0x47) */
	unsigned int target_node;	/* 0 = any remote, otherwise pin to this node */
	unsigned int host_svc;		/* AP-side QMI server svc (0x31) */
	unsigned int host_version;	/* AP-side service version */
	unsigned int host_instance;	/* AP-side service instance */
	unsigned int platform_type;	/* platform_type TLV value */
	bool announce_host;		/* register AP-side QMI server */
	bool verbose;
};

static volatile sig_atomic_t g_stop;

static const struct option long_options[] = {
	{ "target-svc",     required_argument, NULL, 's' },
	{ "target-node",    required_argument, NULL, 'N' },
	{ "host-svc",       required_argument, NULL, 'S' },
	{ "host-version",   required_argument, NULL, 'v' },
	{ "host-instance",  required_argument, NULL, 'i' },
	{ "platform",       required_argument, NULL, 'p' },
	{ "no-announce",    no_argument,       NULL, 'A' },
	{ "verbose",        no_argument,       NULL, 'V' },
	{ "help",           no_argument,       NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(FILE *s, const char *prog)
{
	fprintf(s,
		"Usage: %s [options]\n"
		"\n"
		"AP-side IPA-QMI proxy for external SDX55.\n"
		"\n"
		"  -s, --target-svc N     SDX55 IPA-QMI service to hunt (default: 0x47)\n"
		"  -N, --target-node N    only respond to remote node N (default: any remote)\n"
		"  -S, --host-svc N       AP-side IPA-QMI server svc (default: 0x31)\n"
		"  -v, --host-version N   AP-side service version (default: 1)\n"
		"  -i, --host-instance N  AP-side service instance (default: 1)\n"
		"  -p, --platform N       IPA platform_type TLV (default: 6 = LE_MHI)\n"
		"  -A, --no-announce      do not register AP-side QMI server (client-only)\n"
		"  -V, --verbose          log payload details\n"
		"  -h, --help             this help\n",
		prog);
}

static void ts_log(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%6ld.%06ld] ipa_qmi_proxy: ",
		(long)ts.tv_sec, (long)(ts.tv_nsec / 1000));
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void dump_hex(const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		fprintf(stderr, "%02x", buf[i]);
		if (i + 1 != len)
			fputc(' ', stderr);
	}
	fputc('\n', stderr);
}

static uint32_t pack_instance(unsigned int version, unsigned int instance)
{
	return ((instance & 0x00ffffffu) << 8) | (version & 0xffu);
}

static int announce_server(int fd, unsigned int node, unsigned int port,
			   unsigned int service_id, unsigned int version,
			   unsigned int instance)
{
	struct sockaddr_qrtr addr = {
		.sq_family = AF_QIPCRTR,
		.sq_node = QRTR_NODE_BCAST,
		.sq_port = QRTR_PORT_CTRL,
	};
	struct qrtr_ctrl_pkt_wire pkt = { 0 };
	ssize_t n;

	pkt.cmd            = htole32(QRTR_TYPE_NEW_SERVER);
	pkt.server.service = htole32(service_id);
	pkt.server.instance = htole32(pack_instance(version, instance));
	pkt.server.node    = htole32(node);
	pkt.server.port    = htole32(port);

	n = sendto(fd, &pkt, sizeof(pkt), 0,
		   (const struct sockaddr *)&addr, sizeof(addr));
	return n < 0 ? -errno : 0;
}

/*
 * Subscribe to QRTR control channel (to receive NEW_SERVER / DEL_SERVER /
 * NEW_LOOKUP etc). The kernel sends us a replay of the server list.
 */
static int subscribe_ctrl(int fd)
{
	struct sockaddr_qrtr addr = {
		.sq_family = AF_QIPCRTR,
		.sq_node = QRTR_NODE_BCAST,
		.sq_port = QRTR_PORT_CTRL,
	};
	struct qrtr_ctrl_pkt_wire pkt = { 0 };
	ssize_t n;

	pkt.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
	pkt.server.service = htole32(0);	/* 0 == all services */
	pkt.server.instance = htole32(0);

	n = sendto(fd, &pkt, sizeof(pkt), 0,
		   (const struct sockaddr *)&addr, sizeof(addr));
	return n < 0 ? -errno : 0;
}

static int send_qmi(int fd, const struct sockaddr_qrtr *dst,
		    uint8_t type, uint16_t txn, uint16_t msg_id,
		    const void *payload, uint16_t payload_len)
{
	uint8_t buf[512];
	struct qmi_header hdr;
	ssize_t n;

	if (sizeof(hdr) + payload_len > sizeof(buf))
		return -EMSGSIZE;

	hdr.type    = type;
	hdr.txn_id  = htole16(txn);
	hdr.msg_id  = htole16(msg_id);
	hdr.msg_len = htole16(payload_len);

	memcpy(buf, &hdr, sizeof(hdr));
	if (payload_len)
		memcpy(buf + sizeof(hdr), payload, payload_len);

	n = sendto(fd, buf, sizeof(hdr) + payload_len, 0,
		   (const struct sockaddr *)dst, sizeof(*dst));
	if (n < 0)
		return -errno;
	if ((size_t)n != sizeof(hdr) + payload_len)
		return -EIO;
	return 0;
}

/*
 * Build a minimal IPA INIT_DRIVER (0x21) request payload.
 * Only platform_type and first-boot state are present. The vendor IPA stack
 * uses LE_MHI for MHI offload platforms; plain LE is rejected by SDX55.
 *
 * TLV layout per drivers/net/ipa/ipa_qmi_msg.c ipa_init_modem_driver_req_ei[]:
 *   tlv_type 0x10 (platform_type_valid + platform_type, u32 ENUM)
 *   tlv_type 0x18 (skip_uc_load_valid + skip_uc_load, u8)  -- optional, helpful
 *
 * Wire format: [type:1 | length:2 LE | value:len]
 */
static int build_init_driver_req(uint8_t *buf, size_t cap,
				 unsigned int platform_type)
{
	size_t off = 0;

	if (cap < 16)
		return -EMSGSIZE;

	/* platform_type (u32 enum) */
	buf[off++] = 0x10;				/* tlv_type */
	buf[off++] = 0x04; buf[off++] = 0x00;		/* length = 4 LE */
	buf[off++] = (uint8_t)(platform_type & 0xff);
	buf[off++] = (uint8_t)((platform_type >> 8) & 0xff);
	buf[off++] = (uint8_t)((platform_type >> 16) & 0xff);
	buf[off++] = (uint8_t)((platform_type >> 24) & 0xff);

	/* skip_uc_load = 0 (initial boot, don't skip uC load) (u8) */
	buf[off++] = 0x18;				/* tlv_type */
	buf[off++] = 0x01; buf[off++] = 0x00;		/* length = 1 LE */
	buf[off++] = 0x00;				/* value */

	return (int)off;
}

static int send_init_driver(int fd, const struct sockaddr_qrtr *dst,
			    uint16_t txn, unsigned int platform_type,
			    bool verbose)
{
	uint8_t payload[64];
	int plen;

	plen = build_init_driver_req(payload, sizeof(payload), platform_type);
	if (plen < 0)
		return plen;

	if (verbose) {
		ts_log("sending INIT_DRIVER to %u:%u txn=%u payload_len=%d\n",
		       dst->sq_node, dst->sq_port, txn, plen);
		fprintf(stderr, "  payload: ");
		dump_hex(payload, plen);
	}

	return send_qmi(fd, dst, QMI_REQUEST, txn, IPA_QMI_INIT_DRIVER,
			payload, (uint16_t)plen);
}

/*
 * Standard QMI success response: TLV 0x02 struct qmi_response_type_v01
 *   result = 0 (SUCCESS), error = 0 (NONE)
 * Wire: 02 04 00 00 00 00 00
 */
static int send_success_resp(int fd, const struct sockaddr_qrtr *dst,
			     uint16_t txn, uint16_t msg_id)
{
	static const uint8_t rsp[] = {
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	return send_qmi(fd, dst, QMI_RESPONSE, txn, msg_id,
			rsp, sizeof(rsp));
}

/* INIT_COMPLETE indication (0x22) — only a qmi_response_type_v01 TLV */
static int send_init_complete_ind(int fd, const struct sockaddr_qrtr *dst,
				  uint16_t txn)
{
	static const uint8_t ind[] = {
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	return send_qmi(fd, dst, QMI_INDICATION, txn, IPA_QMI_INIT_COMPLETE,
			ind, sizeof(ind));
}

static bool parse_qmi_result(const uint8_t *payload, size_t len,
			     uint16_t *result, uint16_t *error)
{
	size_t off = 0;

	while (off + 3 <= len) {
		uint8_t type = payload[off++];
		uint16_t tlv_len = payload[off] | ((uint16_t)payload[off + 1] << 8);

		off += 2;
		if (off + tlv_len > len)
			break;

		if (type == 0x02 && tlv_len >= 4) {
			*result = payload[off] | ((uint16_t)payload[off + 1] << 8);
			*error = payload[off + 2] | ((uint16_t)payload[off + 3] << 8);
			return true;
		}

		off += tlv_len;
	}

	return false;
}

int main(int argc, char **argv)
{
	struct config cfg = {
		.target_svc     = IPA_MODEM_SVC_ID_SDX55,
		.target_node    = 0,
		.host_svc       = IPA_HOST_SVC_ID_AP,
		.host_version   = 1,
		.host_instance  = 1,
		.platform_type  = IPA_QMI_PLATFORM_TYPE_LE_MHI,
		.announce_host  = true,
		.verbose        = false,
	};
	struct sockaddr_qrtr local = {
		.sq_family = AF_QIPCRTR,
		.sq_node = 0,
		.sq_port = 0,
	};
	struct sockaddr_qrtr self_addr;
	socklen_t self_len = sizeof(self_addr);
	struct sockaddr_qrtr modem_sq = { 0 };
	bool have_modem = false;
	bool sent_init_driver = false;
	uint16_t next_txn = 1;
	int fd;
	int opt;

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	while ((opt = getopt_long(argc, argv,
				  "s:N:S:v:i:p:AVh", long_options, NULL)) != -1) {
		switch (opt) {
		case 's': cfg.target_svc     = strtoul(optarg, NULL, 0); break;
		case 'N': cfg.target_node    = strtoul(optarg, NULL, 0); break;
		case 'S': cfg.host_svc       = strtoul(optarg, NULL, 0); break;
		case 'v': cfg.host_version   = strtoul(optarg, NULL, 0); break;
		case 'i': cfg.host_instance  = strtoul(optarg, NULL, 0); break;
		case 'p': cfg.platform_type  = strtoul(optarg, NULL, 0); break;
		case 'A': cfg.announce_host  = false; break;
		case 'V': cfg.verbose        = true;  break;
		case 'h': usage(stdout, argv[0]); return 0;
		default:  usage(stderr, argv[0]); return 1;
		}
	}

	fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("socket(AF_QIPCRTR)");
		return 1;
	}

	if (getsockname(fd, (struct sockaddr *)&self_addr, &self_len) < 0) {
		perror("getsockname");
		close(fd);
		return 1;
	}
	local.sq_node = self_addr.sq_node;

	if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}

	if (getsockname(fd, (struct sockaddr *)&local, &self_len) < 0) {
		perror("getsockname post-bind");
		close(fd);
		return 1;
	}

	ts_log("bound local node=%u port=%u\n", local.sq_node, local.sq_port);

	if (cfg.announce_host) {
		if (announce_server(fd, local.sq_node, local.sq_port,
				    cfg.host_svc, cfg.host_version,
				    cfg.host_instance) != 0) {
			perror("announce_server");
			close(fd);
			return 1;
		}
		ts_log("announced AP IPA-QMI server svc=0x%x v=%u inst=%u at node=%u port=%u\n",
		       cfg.host_svc, cfg.host_version, cfg.host_instance,
		       local.sq_node, local.sq_port);
	}

	if (subscribe_ctrl(fd) != 0) {
		perror("subscribe_ctrl (NEW_LOOKUP)");
		close(fd);
		return 1;
	}
	ts_log("subscribed to QRTR control (NEW_LOOKUP); watching for svc=0x%x%s%u\n",
	       cfg.target_svc,
	       cfg.target_node ? " from node=" : " (any node)",
	       cfg.target_node);

	while (!g_stop) {
		uint8_t buf[2048];
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct sockaddr_qrtr peer;
		socklen_t peer_len = sizeof(peer);
		ssize_t n;
		int rc;

		rc = poll(&pfd, 1, 500);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (rc == 0)
			continue;

		n = recvfrom(fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&peer, &peer_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			break;
		}

		/* QRTR CTRL messages (from port QRTR_PORT_CTRL) */
		if (peer.sq_port == QRTR_PORT_CTRL &&
		    (size_t)n >= sizeof(uint32_t)) {
			struct qrtr_ctrl_pkt_wire pkt;
			uint32_t cmd;

			if ((size_t)n < sizeof(pkt))
				continue;
			memcpy(&pkt, buf, sizeof(pkt));
			cmd = le32toh(pkt.cmd);

			if (cmd == QRTR_TYPE_NEW_SERVER) {
				uint32_t svc  = le32toh(pkt.server.service);
				uint32_t inst = le32toh(pkt.server.instance);
				uint32_t node = le32toh(pkt.server.node);
				uint32_t port = le32toh(pkt.server.port);

				/* End-of-replay marker: service=0 node=0 port=0 */
				if (!svc && !node && !port)
					continue;

				if (cfg.verbose) {
					ts_log("CTRL NEW_SERVER svc=0x%x inst=0x%x node=%u port=%u\n",
					       svc, inst, node, port);
				}

				if (svc != cfg.target_svc)
					continue;
				if (node == local.sq_node)
					continue;	/* skip self */
				if (cfg.target_node &&
				    node != cfg.target_node)
					continue;

				modem_sq.sq_family = AF_QIPCRTR;
				modem_sq.sq_node   = node;
				modem_sq.sq_port   = port;
				have_modem = true;

				ts_log("FOUND target svc=0x%x inst=0x%x at node=%u port=%u\n",
				       svc, inst, node, port);

				if (!sent_init_driver) {
					rc = send_init_driver(fd, &modem_sq,
							      next_txn++,
							      cfg.platform_type,
							      cfg.verbose);
					if (rc) {
						ts_log("send INIT_DRIVER failed: %s\n",
						       strerror(-rc));
					} else {
						sent_init_driver = true;
						ts_log("INIT_DRIVER sent to %u:%u\n",
						       modem_sq.sq_node,
						       modem_sq.sq_port);
					}
				}
			} else if (cmd == QRTR_TYPE_DEL_SERVER) {
				uint32_t svc  = le32toh(pkt.server.service);
				uint32_t node = le32toh(pkt.server.node);
				uint32_t port = le32toh(pkt.server.port);

				if (cfg.verbose) {
					ts_log("CTRL DEL_SERVER svc=0x%x node=%u port=%u\n",
					       svc, node, port);
				}
				if (have_modem && svc == cfg.target_svc &&
				    node == modem_sq.sq_node) {
					ts_log("target service %u:%u gone; will re-send INIT_DRIVER on re-advert\n",
					       node, port);
					have_modem = false;
					sent_init_driver = false;
				}
			}
			continue;
		}

		/* QMI messages from a peer endpoint */
		if ((size_t)n < sizeof(struct qmi_header))
			continue;

		struct qmi_header hdr;
		memcpy(&hdr, buf, sizeof(hdr));
		uint8_t  type    = hdr.type;
		uint16_t txn     = le16toh(hdr.txn_id);
		uint16_t msg_id  = le16toh(hdr.msg_id);
		uint16_t msg_len = le16toh(hdr.msg_len);

		if ((size_t)n < sizeof(hdr) + msg_len)
			continue;

		ts_log("QMI %s msg=0x%04x txn=%u len=%u from %u:%u\n",
		       type == QMI_REQUEST ? "REQ" :
		       type == QMI_RESPONSE ? "RSP" :
		       type == QMI_INDICATION ? "IND" : "?",
		       msg_id, txn, msg_len, peer.sq_node, peer.sq_port);

		if (cfg.verbose && msg_len) {
			fprintf(stderr, "  body: ");
			dump_hex(buf + sizeof(hdr), msg_len);
		}

		if (type == QMI_REQUEST) {
			switch (msg_id) {
			case IPA_QMI_INDICATION_REGISTER:
				rc = send_success_resp(fd, &peer, txn, msg_id);
				ts_log("-> INDICATION_REGISTER SUCCESS rsp (rc=%d)\n", rc);
				/* Follow up with INIT_COMPLETE indication */
				rc = send_init_complete_ind(fd, &peer, next_txn++);
				ts_log("-> INIT_COMPLETE ind (rc=%d)\n", rc);
				break;
			case IPA_QMI_DRIVER_INIT_COMPLETE:
				rc = send_success_resp(fd, &peer, txn, msg_id);
				ts_log("-> DRIVER_INIT_COMPLETE SUCCESS rsp (rc=%d)\n", rc);
				break;
			default:
				ts_log("unhandled REQ msg=0x%04x; replying SUCCESS anyway\n",
				       msg_id);
				send_success_resp(fd, &peer, txn, msg_id);
				break;
			}
		} else if (type == QMI_RESPONSE) {
			if (msg_id == IPA_QMI_INIT_DRIVER) {
				uint16_t result = 0xffff;
				uint16_t error = 0xffff;

				if (parse_qmi_result(buf + sizeof(hdr), msg_len,
						     &result, &error) &&
				    result == QMI_RESULT_SUCCESS_V01 &&
				    error == QMI_ERR_NONE_V01) {
					ts_log("INIT_DRIVER response success; handshake leg 1 OK\n");
				} else {
					ts_log("INIT_DRIVER response failure: result=%u error=%u\n",
					       result, error);
				}
			}
		}
	}

	close(fd);
	return 0;
}
