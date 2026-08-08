// SPDX-License-Identifier: GPL-2.0
/*
 * efs_mhi_write - Write to modem EFS through the raw MHI EFS channel.
 *
 * Uses /dev/wwan0qcdm1 (the EFS MHI channel pair 10/11 exposed via WWAN)
 * to send EFS commands directly, bypassing both DIAG and QMI.
 *
 * Protocol v2: matches the inner payload format used by Xiaomi's QMI
 * EFS service (0xffe4) — op, sub_id, u8 path_len, path, then operation-
 * specific payload. No HDLC framing, no QMI TLV wrapping.
 *
 * The modem side is remotefs_sio.c:remotefs_sio_rx_notify() which
 * validates item_ptr and data size before dispatching.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

static volatile int g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double mono_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── EFS command codes (matching xiaomi_efs_probe inner format) ───── */

#define EFS_OP_READ   4
#define EFS_OP_WRITE  5
#define EFS_OP_DELETE 6

#define EFS_WRITE_PATH_MAX   71
#define EFS_WRITE_DATA_MAX   414
#define EFS_WRITE_DLEN_OFF   76
#define EFS_WRITE_CLEN_OFF   80
#define EFS_WRITE_DATA_OFF   88
#define EFS_INNER_BUF_SIZE   512

static void hexdump(const uint8_t *p, size_t len)
{
	fprintf(stderr, "[%9.3f] hex (%zu bytes):", mono_sec(), len);
	for (size_t i = 0; i < len && i < 96; i++)
		fprintf(stderr, " %02x", p[i]);
	if (len > 96)
		fprintf(stderr, " ...");
	fprintf(stderr, "\n");

	for (size_t i = 0; i < len; i++) {
		if (p[i] >= 0x20 && p[i] < 0x7f) {
			size_t start = i;
			while (i < len && p[i] >= 0x20 && p[i] < 0x7f) i++;
			if (i - start >= 4)
				fprintf(stderr, "  str@%zu: \"%.*s\"\n",
					start, (int)(i - start), (const char *)&p[start]);
		}
	}
}

/*
 * Build EFS read command (matches xiaomi_efs_probe inner format):
 *   [op=4][sub_id][padding:2][path_len_u8][path_bytes]
 */
static size_t build_efs_read(uint8_t *buf, size_t cap, uint8_t sub_id, const char *path)
{
	size_t plen = strlen(path);

	if (plen > 255)
		return 0;
	if (cap < 5 + plen)
		return 0;

	buf[0] = EFS_OP_READ;
	buf[1] = sub_id;
	buf[2] = 0;
	buf[3] = 0;
	buf[4] = (uint8_t)plen;
	memcpy(buf + 5, path, plen);
	return 5 + plen;
}

/*
 * Build EFS write command (matches xiaomi_efs_probe inner format):
 *   [op=5][sub_id][padding:2][path_len_u8][path_bytes]
 *   [zeros up to offset 76]
 *   [data_len LE32 @76][clamped_len LE32 @80][reserved LE32 @84]
 *   [data @88]
 */
static size_t build_efs_write(uint8_t *buf, size_t cap, uint8_t sub_id,
			       const char *path, const uint8_t *data, size_t dlen)
{
	size_t plen = strlen(path);
	uint32_t le32;

	if (plen > EFS_WRITE_PATH_MAX)
		return 0;
	if (dlen > EFS_WRITE_DATA_MAX)
		return 0;
	if (cap < EFS_INNER_BUF_SIZE)
		return 0;

	memset(buf, 0, EFS_INNER_BUF_SIZE);
	buf[0] = EFS_OP_WRITE;
	buf[1] = sub_id;
	/* buf[2..3] already zero */
	buf[4] = (uint8_t)plen;
	memcpy(buf + 5, path, plen);

	le32 = htole32((uint32_t)dlen);
	memcpy(buf + EFS_WRITE_DLEN_OFF, &le32, sizeof(le32));
	memcpy(buf + EFS_WRITE_CLEN_OFF, &le32, sizeof(le32));
	/* buf[84..87] stays zero (reserved) */
	memcpy(buf + EFS_WRITE_DATA_OFF, data, dlen);

	return EFS_INNER_BUF_SIZE;
}

static int send_raw(int fd, const uint8_t *payload, size_t plen)
{
	ssize_t n = write(fd, payload, plen);
	if (n < 0) {
		fprintf(stderr, "[%9.3f] write failed: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	if ((size_t)n != plen) {
		fprintf(stderr, "[%9.3f] short write: %zd vs %zu\n", mono_sec(), n, plen);
		return -1;
	}
	return 0;
}

static int recv_raw(int fd, uint8_t *buf, size_t bufsz, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int rc;

	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0) {
		if (errno == EINTR) return 0;
		fprintf(stderr, "[%9.3f] poll error: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	if (rc == 0) {
		fprintf(stderr, "[%9.3f] recv timeout after %dms\n", mono_sec(), timeout_ms);
		return 0;
	}

	ssize_t n = read(fd, buf, bufsz - 1);
	if (n <= 0) {
		if (errno == EAGAIN || errno == EINTR) return 0;
		fprintf(stderr, "[%9.3f] read error: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	buf[n] = 0;
	return (int)n;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	const char *dev = "/dev/wwan0qcdm1";
	const char *path = NULL;
	const char *data_hex = NULL;
	int sub_id = 0;
	int op = 0; /* 0=read, 1=write, 2=delete, 3=passive */
	uint8_t txbuf[65536], rxbuf[65536], data_buf[4096];
	size_t data_len = 0;
	int fd, ret = 1;
	int passive_timeout = 5000;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "-u") && i + 1 < argc)
			sub_id = (uint8_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--read"))
			op = 0;
		else if (!strcmp(argv[i], "--write"))
			op = 1;
		else if (!strcmp(argv[i], "--delete"))
			op = 2;
		else if (!strcmp(argv[i], "--passive"))
			op = 3;
		else if (!strcmp(argv[i], "-T") && i + 1 < argc)
			passive_timeout = atoi(argv[++i]) * 1000;
		else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
			data_hex = argv[++i];
			data_len = strlen(data_hex) / 2;
			if (data_len > sizeof(data_buf)) {
				fprintf(stderr, "data too long\n");
				return 1;
			}
			for (size_t j = 0; j < data_len; j++) {
				unsigned int byte;
				sscanf(data_hex + j * 2, "%02x", &byte);
				data_buf[j] = (uint8_t)byte;
			}
		} else {
			fprintf(stderr,
				"usage: %s [--read|--write|--delete|--passive] [-p <efs_path>] [-u <sub_id>] [-x <hex>] [-d <dev>] [-T <sec>]\n"
				"  --read     read path (default)\n"
				"  --write    write data to path (requires -x)\n"
				"  --delete   delete path\n"
				"  --passive  open device, read-only, dump any data from modem\n"
				"  -u <id>    modem sub-id (default 0)\n"
				"  -d <dev>   device (default: /dev/wwan0qcdm1)\n"
				"  -T <sec>   passive read timeout in seconds (default 5)\n",
				argv[0]);
			return 1;
		}
	}

	if (op != 3 && !path) {
		fprintf(stderr, "error: -p <path> required for non-passive mode\n");
		return 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	fprintf(stderr, "[%9.3f] opened %s\n", mono_sec(), dev);

	switch (op) {
	case 0: { /* read */
		size_t plen = build_efs_read(txbuf, sizeof(txbuf), sub_id, path);
		if (!plen) {
			fprintf(stderr, "READ: build failed\n");
			break;
		}
		fprintf(stderr, "[%9.3f] EFS_READ \"%s\" sub=%u (path_len=%zu payload=%zu)\n",
			mono_sec(), path, sub_id, strlen(path), plen);
		hexdump(txbuf, plen);
		if (send_raw(fd, txbuf, plen) < 0)
			break;
		int n = recv_raw(fd, rxbuf, sizeof(rxbuf), 3000);
		if (n > 0) {
			fprintf(stderr, "[%9.3f] RX %d bytes:\n", mono_sec(), n);
			hexdump(rxbuf, (size_t)n);
			ret = 0;
		} else if (n == 0) {
			fprintf(stderr, "[%9.3f] RX timeout (channel may not send response for reads)\n", mono_sec());
		}
		break;
	}
	case 1: { /* write */
		if (!data_len) {
			fprintf(stderr, "error: --write requires -x <hex>\n");
			break;
		}
		size_t plen = build_efs_write(txbuf, sizeof(txbuf), sub_id,
					      path, data_buf, data_len);
		if (!plen) {
			fprintf(stderr, "WRITE: build failed\n");
			break;
		}
		fprintf(stderr, "[%9.3f] EFS_WRITE \"%s\" sub=%u %zu data bytes (payload=%zu)\n",
			mono_sec(), path, sub_id, data_len, plen);
		if (send_raw(fd, txbuf, plen) < 0)
			break;
		int n = recv_raw(fd, rxbuf, sizeof(rxbuf), 5000);
		if (n > 0) {
			fprintf(stderr, "[%9.3f] RX %d bytes:\n", mono_sec(), n);
			hexdump(rxbuf, (size_t)n);
			ret = 0;
		}
		break;
	}
	case 2: { /* delete */
		size_t plen = build_efs_read(txbuf, sizeof(txbuf), sub_id, path);
		if (!plen) {
			fprintf(stderr, "DELETE: build failed\n");
			break;
		}
		txbuf[0] = EFS_OP_DELETE;
		fprintf(stderr, "[%9.3f] EFS_DELETE \"%s\" sub=%u (%zu bytes)\n",
			mono_sec(), path, sub_id, plen);
		if (send_raw(fd, txbuf, plen) < 0)
			break;
		int n = recv_raw(fd, rxbuf, sizeof(rxbuf), 3000);
		if (n > 0) {
			fprintf(stderr, "[%9.3f] RX %d bytes:\n", mono_sec(), n);
			hexdump(rxbuf, (size_t)n);
			ret = 0;
		}
		break;
	}
	case 3: { /* passive read-only */
		fprintf(stderr, "[%9.3f] PASSIVE: listening for %dms on %s (no write)\n",
			mono_sec(), passive_timeout, dev);
		double deadline = mono_sec() + passive_timeout / 1000.0;
		while (!g_stop) {
			double remain = deadline - mono_sec();
			if (remain <= 0)
				break;
			int ms = (int)(remain * 1000);
			if (ms < 100) ms = 100;
			int n = recv_raw(fd, rxbuf, sizeof(rxbuf), ms);
			if (n < 0)
				break;
			if (n > 0) {
				fprintf(stderr, "[%9.3f] PASSIVE RX %d bytes:\n", mono_sec(), n);
				hexdump(rxbuf, (size_t)n);
			}
		}
		if (!g_stop)
			fprintf(stderr, "[%9.3f] PASSIVE: timeout, no data received\n", mono_sec());
		ret = 0;
		break;
	}
	}

	close(fd);
	return ret;
}
