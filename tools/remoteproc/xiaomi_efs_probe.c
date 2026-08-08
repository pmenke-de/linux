// SPDX-License-Identifier: GPL-2.0-only
/*
 * xiaomi_efs_probe - minimal Xiaomi extend-QMI EFS client.
 *
 * The vendor mtb binary talks to a Xiaomi private QMI service:
 *   service id 0xffe4, version 1, idl version 6
 *
 * mtb sends QMI message id 2 to that service.  Its inner 512-byte payload uses:
 *   byte 0: EFS operation (4 read, 5 write, 6 delete)
 *   byte 1: modem sub-id
 *   byte 4: path length (u8)
 *   byte 5..: path bytes (no NUL, max 255 for read/delete; max 71 for write)
 *
 * For op=5 (write) the rest of the inner buffer carries the data block.  The
 * layout was confirmed by disassembling /vendor/bin/mtb at xiaomi_efs_write
 * (sym near 0x7958c on Apollo's Android image):
 *   bytes 76..79 : data_len (u32 LE) - actual length the caller asked for
 *   bytes 80..83 : clamped_len (u32 LE) - min(data_len, 414); bytes copied
 *   bytes 84..87 : reserved (observed 0 for write)
 *   bytes 88..   : data (clamped_len bytes)
 *
 * QCCI wraps that inner payload in a generated QMI message.  mtb's generated
 * IDL and xiaomi_extend_qmi_send_sync() build:
 *   TLV 0x01: u32 request id (2 for EFS)
 *   TLV 0x02: u32 data length (0x200)
 *   TLV 0x10: fixed 512-byte data block
 *
 * This tool emits that wire format directly over AF_QIPCRTR and prints the
 * raw response TLVs so we can validate it without Android userspace.
 */

#define _GNU_SOURCE

#include <endian.h>
#include <errno.h>
#include <getopt.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define XIAOMI_EXTEND_SVC	0xffe4u
#define XIAOMI_EXTEND_MSG	0x0002u
#define XIAOMI_REQUEST_ID_EFS	0x0002u
#define XIAOMI_REQUEST_ID_CMN	0x0001u
#define XIAOMI_REQUEST_ID_THIRD	0x0004u
#define XIAOMI_NV_READ		1u
#define XIAOMI_NV_WRITE		2u
#define XIAOMI_NV_DELETE	3u
#define XIAOMI_EFS_READ		4u
#define XIAOMI_EFS_WRITE	5u
#define XIAOMI_EFS_DELETE	6u

#define XIAOMI_NV_ID_MAX	0x1c40u
#define XIAOMI_NV_DATA_OFF	0x58u
#define XIAOMI_NV_LEN_OFF	0x4cu
#define XIAOMI_NV_DATA_MAX	0x1a8u

#define QMI_REQUEST		0
#define QMI_RESPONSE		2
#define QMI_INDICATION		4

#define XIAOMI_EXTEND_DATA_LEN	0x200u
#define XIAOMI_EXTEND_CMN_DATA_LEN	0x1acu	/* cmn_handler inner buffer size */
#define XIAOMI_EXTEND_BUF_MAX	0x200u

#define XIAOMI_EFS_WRITE_PATH_MAX	71u
#define XIAOMI_EFS_WRITE_DATA_MAX	414u
#define XIAOMI_EFS_WRITE_DLEN_OFF	76u
#define XIAOMI_EFS_WRITE_CLEN_OFF	80u
#define XIAOMI_EFS_WRITE_DATA_OFF	88u

struct qmi_header {
	uint8_t  type;
	uint16_t txn_id;
	uint16_t msg_id;
	uint16_t msg_len;
} __attribute__((packed));

enum data_len_mode {
	DATA_LEN_VENDOR,
	DATA_LEN_U16,
	DATA_LEN_U32,
	DATA_LEN_RAW,
};

/*
 * Dual-path layout for op=11 (xiaomi_efs_compare) and op=12 (xiaomi_efs_copy).
 * The firmware printf "xiaomi_efs_copy, path_len_src = %d, path_len_tgt = %d"
 * proves a 2-path request. Exact in-buffer layout is not known, so support a
 * few candidate layouts and pick by --dual-layout.
 *   slot80 : path_len_tgt @ BUF[0x80], path_tgt @ BUF[0x81..]   (default)
 *   slot40 : path_len_tgt @ BUF[0x40], path_tgt @ BUF[0x41..]
 *   slot4c : path_len_tgt @ BUF[0x4c], path_tgt @ BUF[0x4d..]   (mirrors op=5
 *            data_len_off used for write payloads)
 *   packed : path_len_tgt @ BUF[5+src_len], path_tgt right after
 *   none   : single-path (default for ops other than 11/12)
 */
enum dual_layout {
	DUAL_NONE,
	DUAL_PACKED,
	DUAL_SLOT40,
	DUAL_SLOT4C,
	DUAL_SLOT80,
};

struct config {
	uint32_t svc;
	uint32_t node;
	uint32_t port;
	uint8_t sub_id;
	uint8_t op;
	enum data_len_mode len_mode;
	int timeout_ms;
	bool verbose;
	bool lookup_only;
	bool nv_mode;
	bool cmn_mode;
	bool third_mode;
	uint16_t nv_id;
	int tlv03_override;
	uint32_t req_id;
	size_t buf_size;
	/* SIM card mode fields */
	bool sim_mode;
	uint8_t sim_sub;
	uint32_t sim_cmd_id;
	uint8_t sim_mode_val;
	uint8_t sim_set_val;
	uint8_t data[XIAOMI_EFS_WRITE_DATA_MAX];
	size_t data_len;
	const char *tgt_path;
	enum dual_layout dual;
};

static int parse_hex_arg(const char *s, uint8_t *out, size_t cap, size_t *outlen)
{
	size_t i = 0, j = 0;

	while (s[i]) {
		unsigned int b;
		char c = s[i];

		if (c == ' ' || c == ':' || c == ',' || c == '\t') {
			i++;
			continue;
		}
		if (!s[i + 1])
			return -EINVAL;
		if (j >= cap)
			return -E2BIG;
		if (sscanf(s + i, "%2x", &b) != 1)
			return -EINVAL;
		out[j++] = (uint8_t)b;
		i += 2;
	}
	*outlen = j;
	return j ? 0 : -EINVAL;
}

static int parse_u32_list_arg(const char *s, uint8_t *out, size_t cap, size_t *outlen)
{
	const char *p = s;
	size_t j = 0;

	while (*p) {
		char *end;
		unsigned long v;
		uint32_t le;

		v = strtoul(p, &end, 0);
		if (end == p)
			return -EINVAL;
		if (v > 0xffffffffUL)
			return -ERANGE;
		if (j + sizeof(le) > cap)
			return -E2BIG;
		le = htole32((uint32_t)v);
		memcpy(out + j, &le, sizeof(le));
		j += sizeof(le);
		if (*end == ',')
			end++;
		else if (*end != '\0')
			return -EINVAL;
		p = end;
	}
	*outlen = j;
	return j ? 0 : -EINVAL;
}

static void usage(FILE *f, const char *prog)
{
	fprintf(f,
		"Usage: %s [options] <efs-path>\n"
		"\n"
		"Read a modem EFS path through Xiaomi extend-QMI service 0xffe4.\n"
		"\n"
		"Options:\n"
		"  -s, --service N     QRTR service id (default: 0xffe4)\n"
		"  -n, --node N        QRTR node; skip lookup if --port is also set\n"
		"  -p, --port N        QRTR port; skip lookup if --node is also set\n"
		"  -u, --sub-id N      Xiaomi EFS sub-id (default: 0)\n"
		"  -o, --op OP         operation: read|write|delete (path-EFS),\n"
		"                       nv_read|nv_write|nv_delete (numeric NV),\n"
		"                       or opN / N for raw op codes 1..9 (default: read)\n"
		"  -N, --nv-id N       numeric NV id (1..%u); switches to NV mode\n"
		"  -C, --cmn-mode      cmn_handler mode: BUF[0..3]=op u32,\n"
		"                       BUF[4..]=--data-hex/--data-u32 bytes;\n"
		"                       implies req-id=1, buf-size=0x1ac\n"
		"  -3, --third-mode    third_handler mode: req_id=4, BUF[0..3]=2,\n"
		"                       BUF[4..7]=0, BUF[8..11]=op u32 LE,\n"
		"                       BUF[12..15]=len, BUF[16..]=payload;\n"
		"                       op passed via --op as decimal integer\n"
		"  -S, --sim-slot N    SIM card slot (0/1); enables SIM provisioning mode\n"
		"  -Q, --sim-cmd-id N  SIM cmd id: 0x36=query, 0x37=get, 0x38=type, 0x39=set\n"
		"  -M, --sim-mode-val N SIM mode value for card_status_set\n"
		"  -V, --sim-set-val N  SIM set value for card_status_set\n"
		"  -F, --tlv03 N       Override flag byte (default: 1 = CMN handler)\n"
		"  -R, --req-id N      QMI TLV 0x01 request id (default: 2 for EFS, 1 for cmn)\n"
		"  -B, --buf-size N    inner buffer size in bytes (default: 0x200 EFS, 0x1ac cmn)\n"
		"  -m, --mode MODE     data TLV mode: vendor, u16, u32, raw (default: vendor)\n"
		"  -t, --timeout MS    response timeout (default: 1200)\n"
		"  -d, --data-hex HEX  bytes to write, e.g. '17000000' or 'aa:bb:cc'\n"
		"  -D, --data-u32 LIST comma-separated u32 values, encoded LE\n"
		"  -L, --lookup-only   only wait for the service and exit\n"
		"  -v, --verbose       dump outgoing packet\n"
		"  -h, --help          show this help\n"
		"\n"
		"DELETE removes the EFS path. WRITE replaces it. Both touch modem flash:\n"
		"only use them with paths you have confirmed by reading first. WRITE caps\n"
		"data at 414 bytes (vendor binary clamp); paths cap at 71 bytes for write.\n"
		"NV ops use numeric id (--nv-id), buffer layout: BUF[0]=op BUF[1]=sub_id\n"
		"BUF[2..3]=nv_id BUF[0x4c]=buf_len BUF[0x58..]=data; data <= %u bytes.\n",
		prog, XIAOMI_NV_ID_MAX, XIAOMI_NV_DATA_MAX);
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void dump_hex(const char *label, const uint8_t *buf, size_t len)
{
	size_t i;

	printf("%s (%zu B):", label, len);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("\n  %04zx:", i);
		printf(" %02x", buf[i]);
	}
	printf("\n");
}

static void print_ascii_preview(const uint8_t *buf, size_t len)
{
	size_t i;

	printf("ascii:");
	for (i = 0; i < len; i++) {
		uint8_t c = buf[i];

		putchar(c >= 0x20 && c < 0x7f ? c : '.');
	}
	putchar('\n');
}

static void alarm_handler(int signum)
{
	(void)signum;
}

static int install_alarm_handler(void)
{
	struct sigaction sa = { 0 };

	sa.sa_handler = alarm_handler;
	sigemptyset(&sa.sa_mask);

	return sigaction(SIGALRM, &sa, NULL) < 0 ? -errno : 0;
}

static int send_new_lookup(int fd, uint32_t svc)
{
	struct sockaddr_qrtr to = {
		.sq_family = AF_QIPCRTR,
		.sq_node = QRTR_NODE_BCAST,
		.sq_port = QRTR_PORT_CTRL,
	};
	struct qrtr_ctrl_pkt pkt = { 0 };
	ssize_t n;

	pkt.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
	pkt.server.service = htole32(svc);
	pkt.server.instance = 0;

	alarm(1);
	n = sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
	alarm(0);
	if (n < 0)
		return -errno;

	return 0;
}

static int lookup_service(int fd, uint32_t svc, uint32_t *node, uint32_t *port,
			  int timeout_ms)
{
	double deadline = now_sec() + timeout_ms / 1000.0;

	for (;;) {
		struct sockaddr_qrtr from;
		socklen_t from_len = sizeof(from);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint8_t buf[512];
		ssize_t n;
		int poll_ret;
		int ret;
		int wait_ms;
		double remain = deadline - now_sec();

		if (remain <= 0)
			return -ETIMEDOUT;

		wait_ms = (int)(remain * 1000.0);
		if (wait_ms < 1)
			wait_ms = 1;

		ret = send_new_lookup(fd, svc);
		if (ret == -EINTR || ret == -EAGAIN || ret == -EWOULDBLOCK ||
		    ret == -ENETRESET || ret == -ENODEV || ret == -ECONNRESET) {
			usleep(100000);
			continue;
		}
		if (ret < 0)
			return ret;

		poll_ret = poll(&pfd, 1, wait_ms > 1000 ? 1000 : wait_ms);
		if (poll_ret < 0)
			return -errno;
		if (!poll_ret)
			continue;
		if (!(pfd.revents & POLLIN))
			continue;

		n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
			     (struct sockaddr *)&from, &from_len);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (n < 0)
			return -errno;

		if (from.sq_port == QRTR_PORT_CTRL &&
		    (size_t)n >= sizeof(struct qrtr_ctrl_pkt)) {
			const struct qrtr_ctrl_pkt *pkt = (const void *)buf;
			uint32_t cmd = le32toh(pkt->cmd);
			uint32_t found_svc = le32toh(pkt->server.service);
			uint32_t found_node = le32toh(pkt->server.node);
			uint32_t found_port = le32toh(pkt->server.port);

			if (cmd == QRTR_TYPE_NEW_SERVER && found_svc == svc &&
			    found_node != 0 && found_port != 0) {
				*node = found_node;
				*port = found_port;
				return 0;
			}
		}
	}
}

static int put_tlv_u32(uint8_t *buf, size_t cap, size_t *off,
		       uint8_t type, uint32_t val)
{
	uint32_t le = htole32(val);

	if (*off + 3 + sizeof(le) > cap)
		return -EMSGSIZE;

	buf[(*off)++] = type;
	buf[(*off)++] = sizeof(le);
	buf[(*off)++] = 0;
	memcpy(buf + *off, &le, sizeof(le));
	*off += sizeof(le);
	return 0;
}

static int put_tlv_data(uint8_t *buf, size_t cap, size_t *off,
			uint8_t type, const uint8_t *data, size_t len,
			enum data_len_mode mode)
{
	size_t len_prefix = mode == DATA_LEN_U32 ? 4 :
			    mode == DATA_LEN_U16 ? 2 : 0;
	uint16_t tlv_len = len_prefix + len;

	if (*off + 3 + tlv_len > cap)
		return -EMSGSIZE;
	if (mode == DATA_LEN_U16 && len > UINT16_MAX)
		return -EINVAL;
	if (mode == DATA_LEN_U32 && len > UINT32_MAX)
		return -EINVAL;

	buf[(*off)++] = type;
	buf[(*off)++] = tlv_len & 0xff;
	buf[(*off)++] = tlv_len >> 8;

	if (mode == DATA_LEN_U16) {
		uint16_t le = htole16((uint16_t)len);

		memcpy(buf + *off, &le, sizeof(le));
		*off += sizeof(le);
	} else if (mode == DATA_LEN_U32) {
		uint32_t le = htole32((uint32_t)len);

		memcpy(buf + *off, &le, sizeof(le));
		*off += sizeof(le);
	}

	memcpy(buf + *off, data, len);
	*off += len;
	return 0;
}

static int put_tlv_fixed_data(uint8_t *buf, size_t cap, size_t *off,
			      uint8_t type, const uint8_t *data, size_t len)
{
	if (len > UINT16_MAX || *off + 3 + len > cap)
		return -EMSGSIZE;

	buf[(*off)++] = type;
	buf[(*off)++] = len & 0xff;
	buf[(*off)++] = len >> 8;
	memcpy(buf + *off, data, len);
	*off += len;

	return 0;
}

static size_t build_nv_inner(uint8_t *buf, size_t cap, uint8_t op,
			     uint8_t sub_id, uint16_t nv_id,
			     const uint8_t *data, size_t data_len)
{
	uint16_t le_nv;
	uint32_t le_len;

	if (cap < XIAOMI_EXTEND_DATA_LEN)
		return 0;
	if (nv_id == 0 || nv_id > XIAOMI_NV_ID_MAX)
		return 0;

	memset(buf, 0, XIAOMI_EXTEND_DATA_LEN);
	buf[0] = op;
	buf[1] = sub_id;
	le_nv = htole16(nv_id);
	memcpy(buf + 2, &le_nv, sizeof(le_nv));

	if (op == XIAOMI_NV_WRITE) {
		if (!data || data_len == 0 || data_len > XIAOMI_NV_DATA_MAX)
			return 0;
		le_len = htole32((uint32_t)data_len);
		memcpy(buf + XIAOMI_NV_LEN_OFF, &le_len, sizeof(le_len));
		memcpy(buf + XIAOMI_NV_DATA_OFF, data, data_len);
	}

	return XIAOMI_EXTEND_DATA_LEN;
}

static size_t build_cmn_inner(uint8_t *buf, size_t cap, size_t buf_size,
			      uint8_t op, const uint8_t *data, size_t data_len)
{
	uint32_t le_op;

	if (cap < buf_size)
		return 0;
	if (data_len + 4 > buf_size)
		return 0;

	memset(buf, 0, buf_size);
	le_op = htole32((uint32_t)op);
	memcpy(buf, &le_op, sizeof(le_op));
	if (data_len)
		memcpy(buf + 4, data, data_len);

	return buf_size;
}

static size_t build_third_inner(uint8_t *buf, size_t cap, uint32_t op,
				const uint8_t *data, size_t data_len)
{
	uint32_t le_const, le_op, le_len;

	if (cap < XIAOMI_EXTEND_DATA_LEN)
		return 0;
	if (data_len + 16 > XIAOMI_EXTEND_DATA_LEN)
		return 0;

	memset(buf, 0, XIAOMI_EXTEND_DATA_LEN);

	le_const = htole32(2u);
	memcpy(buf, &le_const, sizeof(le_const));      /* BUF[0..3] = 2 */
	/* BUF[4..7] = 0 (zeroed by memset) */

	le_op = htole32(op);
	memcpy(buf + 8, &le_op, sizeof(le_op));         /* BUF[8..11] = op */

	le_len = htole32((uint32_t)data_len);
	memcpy(buf + 12, &le_len, sizeof(le_len));      /* BUF[12..15] = data_len */

	if (data_len)
		memcpy(buf + 16, data, data_len);          /* BUF[16..] = payload */

	return XIAOMI_EXTEND_DATA_LEN;
}

/* Build SIM card command inner buffer.
 *
 * Format from xiaomi_cmn_cmd_send (mtb binary):
 *   [0x00] u32 type = 2
 *   [0x04] u32 zero  = 0
 *   [0x08] u32 cmd_id
 *   [0x0c] u32 data_len (payload bytes)
 *   [0x10] u32 payload...
 *
 * Supported cmd_id values:
 *   0x36 = card_status_query (0 args)
 *   0x37 = card_status_get   (1 arg:  sub)
 *   0x38 = card_type_query   (0 args)
 *   0x39 = card_status_set   (3 args: sub, mode, set_val)
 */
static size_t build_sim_inner(uint8_t *buf, size_t cap, size_t buf_size,
			      uint32_t cmd_id, uint8_t sub,
			      uint8_t mode, uint8_t set_val)
{
	uint32_t le;
	uint32_t payload_len, num_args;

	if (cap < buf_size || buf_size < 28)
		return 0;
	memset(buf, 0, buf_size);

	switch (cmd_id) {
	case 0x36:
	case 0x38:
		num_args = 0;
		break;
	case 0x37:
		num_args = 1;
		break;
	case 0x39:
	default:
		num_args = 3;
		break;
	}
	payload_len = num_args * sizeof(uint32_t);

	le = htole32(2);
	memcpy(buf, &le, sizeof(le));
	le = htole32(0);
	memcpy(buf + 4, &le, sizeof(le));
	le = htole32(cmd_id);
	memcpy(buf + 8, &le, sizeof(le));
	le = htole32(payload_len);
	memcpy(buf + 12, &le, sizeof(le));

	if (num_args >= 1) {
		le = htole32((uint32_t)sub);
		memcpy(buf + 16, &le, sizeof(le));
	}
	if (num_args >= 2) {
		le = htole32((uint32_t)mode);
		memcpy(buf + 20, &le, sizeof(le));
	}
	if (num_args >= 3) {
		le = htole32((uint32_t)set_val);
		memcpy(buf + 24, &le, sizeof(le));
	}

	return buf_size;
}

static size_t build_efs_inner(uint8_t *buf, size_t cap, uint8_t op,
			      uint8_t sub_id, const char *path,
			      const uint8_t *data, size_t data_len,
			      const char *tgt_path, enum dual_layout dual)
{
	size_t path_len = strlen(path);
	size_t tgt_len = tgt_path ? strlen(tgt_path) : 0;
	size_t tgt_off = 0;

	if (cap < XIAOMI_EXTEND_DATA_LEN)
		return 0;
	if (path_len > 255)
		return 0;
	if (tgt_path && tgt_len > 255)
		return 0;

	memset(buf, 0, XIAOMI_EXTEND_DATA_LEN);
	buf[0] = op;
	buf[1] = sub_id;
	buf[4] = (uint8_t)path_len;

	if (op == XIAOMI_EFS_WRITE) {
		uint32_t le_dlen, le_clen;

		if (path_len > XIAOMI_EFS_WRITE_PATH_MAX)
			return 0;
		if (!data || data_len == 0)
			return 0;
		if (data_len > XIAOMI_EFS_WRITE_DATA_MAX)
			return 0;

		memcpy(buf + 5, path, path_len);
		le_dlen = htole32((uint32_t)data_len);
		le_clen = htole32((uint32_t)data_len);
		memcpy(buf + XIAOMI_EFS_WRITE_DLEN_OFF, &le_dlen, sizeof(le_dlen));
		memcpy(buf + XIAOMI_EFS_WRITE_CLEN_OFF, &le_clen, sizeof(le_clen));
		memcpy(buf + XIAOMI_EFS_WRITE_DATA_OFF, data, data_len);
		return XIAOMI_EFS_WRITE_DATA_OFF + data_len;
	}

	memcpy(buf + 5, path, path_len);

	if (tgt_path && dual != DUAL_NONE) {
		switch (dual) {
		case DUAL_PACKED:
			tgt_off = 5 + path_len;
			break;
		case DUAL_SLOT40:
			tgt_off = 0x40;
			break;
		case DUAL_SLOT4C:
			tgt_off = 0x4c;
			break;
		case DUAL_SLOT80:
		default:
			tgt_off = 0x80;
			break;
		}
		if (tgt_off + 1 + tgt_len > XIAOMI_EXTEND_DATA_LEN)
			return 0;
		buf[tgt_off] = (uint8_t)tgt_len;
		memcpy(buf + tgt_off + 1, tgt_path, tgt_len);
		return tgt_off + 1 + tgt_len;
	}

	return 5 + path_len;
}

static ssize_t send_efs_request(int fd, uint32_t node, uint32_t port,
				uint16_t txn, uint8_t op, uint8_t sub_id,
				const char *path, uint16_t nv_id, bool nv_mode,
				bool cmn_mode, bool third_mode,
				bool sim_mode, uint32_t sim_cmd_id,
				uint8_t sim_mode_val, uint8_t sim_set_val,
				uint32_t req_id, size_t buf_size,
				const uint8_t *data, size_t data_len,
				enum data_len_mode mode, bool verbose,
				int tlv03_override,
				const char *tgt_path, enum dual_layout dual)
{
	struct sockaddr_qrtr to = {
		.sq_family = AF_QIPCRTR,
		.sq_node = node,
		.sq_port = port,
	};
	uint8_t inner[XIAOMI_EXTEND_BUF_MAX];
	uint8_t pkt[640];
	struct qmi_header hdr;
	size_t inner_len, off;
	ssize_t n;

	if (buf_size > sizeof(inner))
		return -EMSGSIZE;

	memset(inner, 0, sizeof(inner));
	if (sim_mode)
		inner_len = build_sim_inner(inner, sizeof(inner), buf_size,
					    sim_cmd_id, sub_id,
					    sim_mode_val, sim_set_val);
	else if (third_mode)
		inner_len = build_third_inner(inner, sizeof(inner), (uint32_t)op,
					      data, data_len);
	else if (cmn_mode)
		inner_len = build_cmn_inner(inner, sizeof(inner), buf_size, op,
					    data, data_len);
	else if (nv_mode)
		inner_len = build_nv_inner(inner, sizeof(inner), op, sub_id,
					   nv_id, data, data_len);
	else
		inner_len = build_efs_inner(inner, sizeof(inner), op, sub_id,
					    path, data, data_len,
					    tgt_path, dual);
	if (!inner_len)
		return -EINVAL;

	off = sizeof(hdr);

	if (sim_mode) {
		uint32_t le;
		uint8_t flag;

		if (off + 9 + XIAOMI_EXTEND_DATA_LEN > sizeof(pkt))
			return -EMSGSIZE;
		le = htole32(req_id);
		memcpy(pkt + off, &le, sizeof(le));
		off += sizeof(le);
		le = htole32(XIAOMI_EXTEND_DATA_LEN);
		memcpy(pkt + off, &le, sizeof(le));
		off += sizeof(le);
		flag = (uint8_t)(tlv03_override >= 0 ? tlv03_override : 1u);
		pkt[off++] = flag;
		memcpy(pkt + off, inner, XIAOMI_EXTEND_DATA_LEN);
		off += XIAOMI_EXTEND_DATA_LEN;
	} else {
		if (put_tlv_u32(pkt, sizeof(pkt), &off, 0x01, req_id) < 0)
			return -EMSGSIZE;
		if (mode == DATA_LEN_VENDOR) {
			if (put_tlv_u32(pkt, sizeof(pkt), &off, 0x02,
					(uint32_t)buf_size) < 0)
				return -EMSGSIZE;
			if (put_tlv_fixed_data(pkt, sizeof(pkt), &off, 0x10, inner,
					       XIAOMI_EXTEND_DATA_LEN) < 0)
				return -EMSGSIZE;
		} else if (put_tlv_data(pkt, sizeof(pkt), &off, 0x10, inner, inner_len, mode) < 0) {
			return -EMSGSIZE;
		}
	}

	hdr.type = QMI_REQUEST;
	hdr.txn_id = htole16(txn);
	hdr.msg_id = htole16(XIAOMI_EXTEND_MSG);
	hdr.msg_len = htole16((uint16_t)(off - sizeof(hdr)));
	memcpy(pkt, &hdr, sizeof(hdr));

	if (verbose)
		dump_hex("tx", pkt, off);

	n = sendto(fd, pkt, off, 0, (struct sockaddr *)&to, sizeof(to));
	return n < 0 ? -errno : n;
}

static void parse_response_payload(const uint8_t *payload, size_t len)
{
	size_t off = 0;

	while (off + 3 <= len) {
		uint8_t type = payload[off++];
		uint16_t tlv_len = payload[off] | (payload[off + 1] << 8);
		const uint8_t *val;

		off += 2;
		if (off + tlv_len > len) {
			printf("tlv 0x%02x truncated len=%u remaining=%zu\n",
			       type, tlv_len, len - off);
			return;
		}

		val = payload + off;
		printf("tlv 0x%02x len=%u", type, tlv_len);
		if (tlv_len >= 4) {
			uint32_t v;

			memcpy(&v, val, sizeof(v));
			printf(" u32=0x%08x", le32toh(v));
			if (tlv_len == 4) {
				uint16_t lo = val[0] | (val[1] << 8);
				uint16_t hi = val[2] | (val[3] << 8);

				printf(" u16[0]=%u u16[1]=%u", lo, hi);
			}
		}
		printf("\n");

		if (type == 0x10 && tlv_len > 0) {
			uint16_t u16_len = tlv_len >= 2 ? val[0] | (val[1] << 8) : 0;
			uint32_t u32_len = 0;

			if (tlv_len >= 4)
				memcpy(&u32_len, val, sizeof(u32_len));
			u32_len = le32toh(u32_len);

			if (u16_len <= tlv_len - 2) {
				printf("  as u16-array len=%u\n", u16_len);
				dump_hex("  data", val + 2, u16_len);
				print_ascii_preview(val + 2, u16_len);
			}
			if (tlv_len >= 4 && u32_len <= (uint32_t)(tlv_len - 4)) {
				printf("  as u32-array len=%u\n", u32_len);
				dump_hex("  data", val + 4, u32_len);
				print_ascii_preview(val + 4, u32_len);
			}
			if (tlv_len == XIAOMI_EXTEND_DATA_LEN ||
			    tlv_len == XIAOMI_EXTEND_CMN_DATA_LEN) {
				size_t dump = 256;

				if (dump > tlv_len)
					dump = tlv_len;
				printf("  as fixed-%u data preview len=%zu\n",
				       tlv_len, dump);
				dump_hex("  data", val, dump);
				print_ascii_preview(val, dump);
			}
		}

		off += tlv_len;
	}
}

static int recv_response(int fd, uint16_t txn, int timeout_ms)
{
	double deadline = now_sec() + timeout_ms / 1000.0;

	for (;;) {
		struct sockaddr_qrtr from;
		socklen_t from_len = sizeof(from);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint8_t buf[4096];
		const struct qmi_header *hdr;
		uint16_t rxn, msg, msg_len;
		ssize_t n;
		int wait_ms;
		double remain = deadline - now_sec();

		if (remain <= 0)
			return -ETIMEDOUT;
		wait_ms = (int)(remain * 1000.0);
		if (wait_ms < 1)
			wait_ms = 1;

		if (poll(&pfd, 1, wait_ms) <= 0)
			return -ETIMEDOUT;
		if (!(pfd.revents & POLLIN))
			continue;

		n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
			     (struct sockaddr *)&from, &from_len);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (n < 0)
			return -errno;

		if ((size_t)n < sizeof(*hdr))
			continue;
		hdr = (const void *)buf;
		rxn = le16toh(hdr->txn_id);
		msg = le16toh(hdr->msg_id);
		msg_len = le16toh(hdr->msg_len);

		if (hdr->type != QMI_RESPONSE || rxn != txn || msg != XIAOMI_EXTEND_MSG)
			continue;

		printf("rx from %u:%u type=%u txn=%u msg=0x%04x msg_len=%u total=%zd\n",
		       from.sq_node, from.sq_port, hdr->type, rxn, msg, msg_len, n);
		if (sizeof(*hdr) + msg_len <= (size_t)n)
			parse_response_payload(buf + sizeof(*hdr), msg_len);
		else
			printf("short response: header msg_len=%u total=%zd\n", msg_len, n);
		return 0;
	}
}

int main(int argc, char **argv)
{
	struct config cfg = {
		.svc = XIAOMI_EXTEND_SVC,
		.sub_id = 0,
		.op = XIAOMI_EFS_READ,
		.len_mode = DATA_LEN_VENDOR,
		.timeout_ms = 1200,
		.req_id = XIAOMI_REQUEST_ID_EFS,
		.buf_size = XIAOMI_EXTEND_DATA_LEN,
		.third_mode = false,
		.tlv03_override = -1,
	};
	struct sockaddr_qrtr self;
	struct timeval snd_timeout = { .tv_sec = 1 };
	socklen_t self_len = sizeof(self);
	const char *path;
	int fd, ret, opt;

	static const struct option opts[] = {
		{ "service", required_argument, NULL, 's' },
		{ "node", required_argument, NULL, 'n' },
		{ "port", required_argument, NULL, 'p' },
		{ "sub-id", required_argument, NULL, 'u' },
		{ "op", required_argument, NULL, 'o' },
		{ "mode", required_argument, NULL, 'm' },
		{ "timeout", required_argument, NULL, 't' },
		{ "data-hex", required_argument, NULL, 'd' },
		{ "data-u32", required_argument, NULL, 'D' },
		{ "nv-id", required_argument, NULL, 'N' },
		{ "req-id", required_argument, NULL, 'R' },
		{ "buf-size", required_argument, NULL, 'B' },
		{ "cmn-mode", no_argument, NULL, 'C' },
		{ "sim-slot", required_argument, NULL, 'S' },
		{ "sim-cmd-id", required_argument, NULL, 'Q' },
		{ "sim-mode-val", required_argument, NULL, 'M' },
		{ "sim-set-val", required_argument, NULL, 'V' },
		{ "tlv03", required_argument, NULL, 'F' },
		{ "third-mode", no_argument, NULL, '3' },
		{ "lookup-only", no_argument, NULL, 'L' },
		{ "tgt-path", required_argument, NULL, 'T' },
		{ "dual-layout", required_argument, NULL, 1001 },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((opt = getopt_long(argc, argv, "s:n:p:u:o:m:t:d:D:N:R:B:C3S:Q:M:V:LF:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			cfg.svc = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'n':
			cfg.node = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			cfg.port = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'u':
			cfg.sub_id = (uint8_t)strtoul(optarg, NULL, 0);
			break;
		case 'o':
			if (!strcmp(optarg, "read")) {
				cfg.op = XIAOMI_EFS_READ;
			} else if (!strcmp(optarg, "delete")) {
				cfg.op = XIAOMI_EFS_DELETE;
			} else if (!strcmp(optarg, "write")) {
				cfg.op = XIAOMI_EFS_WRITE;
			} else if (!strcmp(optarg, "nv_read")) {
				cfg.op = XIAOMI_NV_READ;
				cfg.nv_mode = true;
			} else if (!strcmp(optarg, "nv_write")) {
				cfg.op = XIAOMI_NV_WRITE;
				cfg.nv_mode = true;
			} else if (!strcmp(optarg, "nv_delete")) {
				cfg.op = XIAOMI_NV_DELETE;
				cfg.nv_mode = true;
			} else {
				const char *p = optarg;
				char *end;
				unsigned long v;

				if (!strncmp(p, "op", 2))
					p += 2;
				v = strtoul(p, &end, 0);
				if (*p == '\0' || *end != '\0' || v > 0xff) {
					fprintf(stderr, "unknown op: %s\n", optarg);
					return 2;
				}
				cfg.op = (uint8_t)v;
			}
			break;
		case 'd':
			if (cfg.data_len) {
				fprintf(stderr,
					"xiaomi_efs_probe: --data-hex/--data-u32 already set\n");
				return 2;
			}
			ret = parse_hex_arg(optarg, cfg.data, sizeof(cfg.data),
					    &cfg.data_len);
			if (ret < 0) {
				fprintf(stderr, "bad --data-hex: %s\n",
					strerror(-ret));
				return 2;
			}
			break;
		case 'D':
			if (cfg.data_len) {
				fprintf(stderr,
					"xiaomi_efs_probe: --data-hex/--data-u32 already set\n");
				return 2;
			}
			ret = parse_u32_list_arg(optarg, cfg.data, sizeof(cfg.data),
						 &cfg.data_len);
			if (ret < 0) {
				fprintf(stderr, "bad --data-u32: %s\n",
					strerror(-ret));
				return 2;
			}
			break;
		case 'm':
			if (!strcmp(optarg, "vendor"))
				cfg.len_mode = DATA_LEN_VENDOR;
			else if (!strcmp(optarg, "u16"))
				cfg.len_mode = DATA_LEN_U16;
			else if (!strcmp(optarg, "u32"))
				cfg.len_mode = DATA_LEN_U32;
			else if (!strcmp(optarg, "raw"))
				cfg.len_mode = DATA_LEN_RAW;
			else {
				fprintf(stderr, "unknown mode: %s\n", optarg);
				return 2;
			}
			break;
		case 't':
			cfg.timeout_ms = atoi(optarg);
			break;
		case 'N': {
			char *end;
			unsigned long v;

			v = strtoul(optarg, &end, 0);
			if (*optarg == '\0' || *end != '\0' ||
			    v == 0 || v > XIAOMI_NV_ID_MAX) {
				fprintf(stderr, "bad --nv-id: %s (1..%u)\n",
					optarg, XIAOMI_NV_ID_MAX);
				return 2;
			}
			cfg.nv_id = (uint16_t)v;
			cfg.nv_mode = true;
			break;
		}
		case 'R': {
			char *end;
			unsigned long v;

			v = strtoul(optarg, &end, 0);
			if (*optarg == '\0' || *end != '\0' || v > 0xffffffffUL) {
				fprintf(stderr, "bad --req-id: %s\n", optarg);
				return 2;
			}
			cfg.req_id = (uint32_t)v;
			break;
		}
		case 'B': {
			char *end;
			unsigned long v;

			v = strtoul(optarg, &end, 0);
			if (*optarg == '\0' || *end != '\0' ||
			    v == 0 || v > XIAOMI_EXTEND_BUF_MAX) {
				fprintf(stderr, "bad --buf-size: %s (1..%u)\n",
					optarg, XIAOMI_EXTEND_BUF_MAX);
				return 2;
			}
			cfg.buf_size = (size_t)v;
			break;
		}
		case 'C':
			cfg.cmn_mode = true;
			if (cfg.req_id == XIAOMI_REQUEST_ID_EFS)
				cfg.req_id = XIAOMI_REQUEST_ID_CMN;
			if (cfg.buf_size == XIAOMI_EXTEND_DATA_LEN)
				cfg.buf_size = XIAOMI_EXTEND_CMN_DATA_LEN;
			break;
		case 'S':
			cfg.sim_mode = true;
			cfg.sim_sub = (uint8_t)strtoul(optarg, NULL, 0);
			cfg.cmn_mode = true;
			cfg.sub_id = cfg.sim_sub;
			cfg.req_id = XIAOMI_REQUEST_ID_THIRD;
			cfg.buf_size = XIAOMI_EXTEND_CMN_DATA_LEN;
			break;
		case 'Q':
			cfg.sim_cmd_id = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'M':
			cfg.sim_mode_val = (uint8_t)strtoul(optarg, NULL, 0);
			break;
		case 'V':
			cfg.sim_set_val = (uint8_t)strtoul(optarg, NULL, 0);
			break;
		case 'F':
			cfg.tlv03_override = atoi(optarg);
			break;
		case '3':
			cfg.third_mode = true;
			if (cfg.req_id == XIAOMI_REQUEST_ID_EFS)
				cfg.req_id = XIAOMI_REQUEST_ID_THIRD;
			if (cfg.buf_size == XIAOMI_EXTEND_DATA_LEN)
				cfg.buf_size = XIAOMI_EXTEND_DATA_LEN;
			break;
		case 'L':
			cfg.lookup_only = true;
			break;
		case 'T':
			cfg.tgt_path = optarg;
			if (cfg.dual == DUAL_NONE)
				cfg.dual = DUAL_SLOT80;
			break;
		case 1001:
			if (!strcmp(optarg, "none"))
				cfg.dual = DUAL_NONE;
			else if (!strcmp(optarg, "packed"))
				cfg.dual = DUAL_PACKED;
			else if (!strcmp(optarg, "slot40"))
				cfg.dual = DUAL_SLOT40;
			else if (!strcmp(optarg, "slot4c"))
				cfg.dual = DUAL_SLOT4C;
			else if (!strcmp(optarg, "slot80"))
				cfg.dual = DUAL_SLOT80;
			else {
				fprintf(stderr, "unknown --dual-layout: %s\n",
					optarg);
				return 2;
			}
			break;
		case 'v':
			cfg.verbose = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}

	if (!cfg.lookup_only && !cfg.nv_mode && !cfg.cmn_mode &&
	    !cfg.third_mode && optind >= argc) {
		usage(stderr, argv[0]);
		return 2;
	}
	if (cfg.lookup_only)
		path = NULL;
	else if (cfg.nv_mode || cfg.cmn_mode || cfg.third_mode)
		path = optind < argc ? argv[optind] : "";
	else
		path = argv[optind];

	if (cfg.nv_mode && cfg.nv_id == 0) {
		fprintf(stderr,
			"xiaomi_efs_probe: nv ops require --nv-id N (1..%u)\n",
			XIAOMI_NV_ID_MAX);
		return 2;
	}
	if (cfg.op == XIAOMI_EFS_WRITE && cfg.data_len == 0) {
		fprintf(stderr,
			"xiaomi_efs_probe: --op write requires --data-hex or --data-u32\n");
		return 2;
	}
	if (cfg.op == XIAOMI_NV_WRITE && cfg.data_len == 0) {
		fprintf(stderr,
			"xiaomi_efs_probe: --op nv_write requires --data-hex or --data-u32\n");
		return 2;
	}
	if (cfg.op == XIAOMI_EFS_READ && cfg.data_len) {
		fprintf(stderr,
			"xiaomi_efs_probe: data only meaningful with non-read ops\n");
		return 2;
	}

	ret = install_alarm_handler();
	if (ret < 0) {
		fprintf(stderr, "xiaomi_efs_probe: sigaction failed: %s\n",
			strerror(-ret));
		return 1;
	}

	fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		perror("socket(AF_QIPCRTR)");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout,
		   sizeof(snd_timeout));

	if (getsockname(fd, (struct sockaddr *)&self, &self_len) == 0)
		fprintf(stderr, "xiaomi_efs_probe: local qrtr %u:%u\n",
			self.sq_node, self.sq_port);

	if (!cfg.node || !cfg.port) {
		ret = lookup_service(fd, cfg.svc, &cfg.node, &cfg.port, cfg.timeout_ms);
		if (ret < 0) {
			fprintf(stderr, "xiaomi_efs_probe: service 0x%x lookup failed: %s\n",
				cfg.svc, strerror(-ret));
			close(fd);
			return 1;
		}
	}

	if (cfg.third_mode)
		fprintf(stderr, "xiaomi_efs_probe: service 0x%x at %u:%u, op=0x%x req_id=%u (third) data_len=%zu\n",
			cfg.svc, cfg.node, cfg.port, cfg.op, cfg.req_id,
			cfg.data_len);
	else if (cfg.cmn_mode)
		fprintf(stderr, "xiaomi_efs_probe: service 0x%x at %u:%u, op=%u req_id=%u buf_size=0x%zx (cmn)\n",
			cfg.svc, cfg.node, cfg.port, cfg.op, cfg.req_id,
			cfg.buf_size);
	else if (cfg.nv_mode)
		fprintf(stderr, "xiaomi_efs_probe: service 0x%x at %u:%u, nv_id=%u sub=%u op=%u (numeric NV)\n",
			cfg.svc, cfg.node, cfg.port, cfg.nv_id,
			cfg.sub_id, cfg.op);
	else if (cfg.tgt_path) {
		static const char *layout_name[] = {
			[DUAL_NONE] = "none", [DUAL_PACKED] = "packed",
			[DUAL_SLOT40] = "slot40", [DUAL_SLOT4C] = "slot4c",
			[DUAL_SLOT80] = "slot80",
		};
		fprintf(stderr, "xiaomi_efs_probe: service 0x%x at %u:%u, src=%s tgt=%s sub=%u op=%u dual=%s\n",
			cfg.svc, cfg.node, cfg.port,
			path ? path : "(none)", cfg.tgt_path,
			cfg.sub_id, cfg.op, layout_name[cfg.dual]);
	} else
		fprintf(stderr, "xiaomi_efs_probe: service 0x%x at %u:%u, path=%s sub=%u op=%u\n",
			cfg.svc, cfg.node, cfg.port, path ? path : "(lookup-only)",
			cfg.sub_id, cfg.op);

	if (cfg.lookup_only) {
		close(fd);
		return 0;
	}

	ret = send_efs_request(fd, cfg.node, cfg.port, 1, cfg.op, cfg.sub_id,
			       path, cfg.nv_id, cfg.nv_mode,
			       cfg.cmn_mode, cfg.third_mode,
			       cfg.sim_mode, cfg.sim_cmd_id,
			       cfg.sim_mode_val, cfg.sim_set_val,
			       cfg.req_id, cfg.buf_size,
			       cfg.data, cfg.data_len,
			       cfg.len_mode, cfg.verbose,
			       cfg.tlv03_override,
			       cfg.tgt_path, cfg.dual);
	if (ret < 0) {
		fprintf(stderr, "xiaomi_efs_probe: send failed: %s\n", strerror(-ret));
		close(fd);
		return 1;
	}

	ret = recv_response(fd, 1, cfg.timeout_ms);
	if (ret < 0) {
		fprintf(stderr, "xiaomi_efs_probe: response failed: %s\n", strerror(-ret));
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
