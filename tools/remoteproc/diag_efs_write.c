// SPDX-License-Identifier: GPL-2.0
/*
 * diag_efs_write - Write to modem EFS through DIAG channel (/dev/wwan0qcdm0).
 *
 * Uses DIAG_CMD_FS_OP (0x59) to perform EFS operations: STAT, OPEN, WRITE,
 * CLOSE.  Bypasses the broken QMI 0xffe4 EFS write path.
 *
 * Protocol:
 *   DIAG HDLC framing: no leading 0x7E, escaped payload, CRC-16/CCITT,
 *   trailing 0x7E.  Built on the same framing validated in diag_reader.c.
 *
 *   DIAG_CMD_FS_OP format:
 *     [0x59] [fs_op] [params...]
 *
 *   FS_OP_STAT  (2): [path_len_LE16] [path...]
 *   FS_OP_OPEN  (0): [mode_LE16] [path_len_LE16] [path...]
 *   FS_OP_WRITE (3): [fd_LE16] [data_len_LE16] [data...]
 *   FS_OP_CLOSE (1): [fd_LE16]
 *   FS_OP_DELETE(5): [path_len_LE16] [path...]
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
#include <sys/time.h>

static volatile int g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double mono_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── HDLC framing (same as diag_reader.c) ─────────────────────────── */

#define HDLC_FLAG  0x7E
#define HDLC_ESC   0x7D
#define HDLC_ESC_MASK 0x20

static uint16_t crc16(const uint8_t *buf, size_t len)
{
	static const uint16_t crc_table[256] = {
		0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
		0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
		0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
		0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
		0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
		0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
		0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
		0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
		0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
		0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
		0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
		0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
		0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
		0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
		0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
		0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
		0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
		0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
		0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
		0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
		0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
		0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
		0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
		0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
		0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
		0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
		0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
		0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
		0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
		0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
		0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
		0x7bc7, 0x6a4e, 0x58d5, 0x4954, 0x3deb, 0x2c62, 0x1ef9, 0x0f70,
	};
	uint16_t crc = 0xFFFF;

	while (len--)
		crc = crc_table[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static int hdlc_encode(const uint8_t *payload, size_t plen,
		       uint8_t *out, size_t out_sz)
{
	uint16_t crc;
	size_t pos = 0;
	size_t i;
	uint8_t crc_bytes[2];

	crc = crc16(payload, plen);
	crc_bytes[0] = crc & 0xFF;
	crc_bytes[1] = (crc >> 8) & 0xFF;

	for (i = 0; i < plen && pos + 3 < out_sz; i++) {
		if (payload[i] == HDLC_FLAG || payload[i] == HDLC_ESC) {
			out[pos++] = HDLC_ESC;
			out[pos++] = payload[i] ^ HDLC_ESC_MASK;
		} else {
			out[pos++] = payload[i];
		}
	}
	for (i = 0; i < 2 && pos + 2 < out_sz; i++) {
		if (crc_bytes[i] == HDLC_FLAG || crc_bytes[i] == HDLC_ESC) {
			out[pos++] = HDLC_ESC;
			out[pos++] = crc_bytes[i] ^ HDLC_ESC_MASK;
		} else {
			out[pos++] = crc_bytes[i];
		}
	}
	if (pos < out_sz) out[pos++] = HDLC_FLAG;
	return (int)pos;
}

/* ── DIAG commands ────────────────────────────────────────────────── */

#define DIAG_CMD_FS_OP          0x59  /* 89 - EFS operations (rejected by SDX55) */
#define DIAG_CMD_DIAG_SUBSYS    0x4B  /* subsystem dispatcher — works on SDX55 */
#define DIAG_SUBSYS_FS          0x13  /* 19 - File System (EFS2) */

/* FS operation codes (packed as LE16 subcmd in subsystem path, or byte in FS_OP) */
#define FS_OP_OPEN    0
#define FS_OP_CLOSE   1
#define FS_OP_READ    2
#define FS_OP_WRITE   3
#define FS_OP_DELETE  5
#define FS_OP_STAT    8
#define FS_OP_STAT2   9   /* fstat — stat by fd */

#define USE_SUBSYS_CMD  1

/* Open modes */
#define FS_MODE_RDONLY   0x0000
#define FS_MODE_WRONLY   0x0001
#define FS_MODE_RDWR     0x0002
#define FS_MODE_CREATE   0x0200
#define FS_MODE_TRUNC    0x0400

/* DIAG response codes */
#define DIAG_BAD_CMD_F   0x13
#define DIAG_BAD_PARM_F  0x14

static void hexdump(const uint8_t *p, size_t len)
{
	fprintf(stderr, "[%9.3f] hex (%zu bytes):", mono_sec(), len);
	for (size_t i = 0; i < len && i < 96; i++)
		fprintf(stderr, " %02x", p[i]);
	if (len > 96)
		fprintf(stderr, " ...");
	fprintf(stderr, "\n");
}

static int send_frame(int fd, const uint8_t *payload, size_t plen)
{
	uint8_t frame[65536];
	int enc;

	enc = hdlc_encode(payload, plen, frame, sizeof(frame));
	if (enc < 0) {
		fprintf(stderr, "[%9.3f] hdlc_encode failed\n", mono_sec());
		return -1;
	}

	ssize_t n = write(fd, frame, enc);
	if (n < 0) {
		fprintf(stderr, "[%9.3f] write failed: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	if ((size_t)n != (size_t)enc) {
		fprintf(stderr, "[%9.3f] short write: %zd vs %d\n", mono_sec(), n, enc);
		return -1;
	}
	return 0;
}

static int recv_response(int fd, uint8_t *buf, size_t bufsz, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	ssize_t n;
	int rc;

	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0) {
		if (errno == EINTR)
			return 0;
		fprintf(stderr, "[%9.3f] poll error: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	if (rc == 0) {
		fprintf(stderr, "[%9.3f] recv timeout after %dms\n", mono_sec(), timeout_ms);
		return 0;
	}

	n = read(fd, buf, bufsz - 1);
	if (n <= 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		fprintf(stderr, "[%9.3f] read error: %s\n", mono_sec(), strerror(errno));
		return -1;
	}
	buf[n] = 0;
	return (int)n;
}

/* ── Subsystem FS command builders ────────────────────────────────── */
/*
 * All FS commands use: [0x4B][0x13][fs_op_LE16][params...]
 */

static size_t build_fs_open(uint8_t *buf, size_t cap,
			    const char *path, uint16_t mode)
{
	size_t plen = strlen(path) + 1;

	if (cap < 4 + 2 + 2 + plen)
		return 0;

	buf[0] = DIAG_CMD_DIAG_SUBSYS;  /* 0x4B */
	buf[1] = DIAG_SUBSYS_FS;        /* 0x13 */
	buf[2] = FS_OP_OPEN & 0xFF;     /* subcmd LSB */
	buf[3] = (FS_OP_OPEN >> 8) & 0xFF; /* subcmd MSB = 0 */
	buf[4] = mode & 0xFF;
	buf[5] = (mode >> 8) & 0xFF;
	buf[6] = plen & 0xFF;
	buf[7] = (plen >> 8) & 0xFF;
	memcpy(buf + 8, path, plen);
	return 8 + plen;
}

static size_t build_fs_close(uint8_t *buf, size_t cap, uint16_t fd)
{
	if (cap < 6)
		return 0;

	buf[0] = DIAG_CMD_DIAG_SUBSYS;
	buf[1] = DIAG_SUBSYS_FS;
	buf[2] = FS_OP_CLOSE & 0xFF;
	buf[3] = (FS_OP_CLOSE >> 8) & 0xFF;
	buf[4] = fd & 0xFF;
	buf[5] = (fd >> 8) & 0xFF;
	return 6;
}

static size_t build_fs_stat(uint8_t *buf, size_t cap, const char *path)
{
	size_t plen = strlen(path) + 1;

	if (cap < 4 + 2 + plen)
		return 0;

	buf[0] = DIAG_CMD_DIAG_SUBSYS;
	buf[1] = DIAG_SUBSYS_FS;
	buf[2] = FS_OP_STAT & 0xFF;
	buf[3] = (FS_OP_STAT >> 8) & 0xFF;
	buf[4] = plen & 0xFF;
	buf[5] = (plen >> 8) & 0xFF;
	memcpy(buf + 6, path, plen);
	return 6 + plen;
}

static size_t build_fs_write(uint8_t *buf, size_t cap,
			     uint16_t fd, const uint8_t *data, size_t dlen)
{
	if (cap < 4 + 2 + 2 + dlen || dlen > 65535)
		return 0;

	buf[0] = DIAG_CMD_DIAG_SUBSYS;
	buf[1] = DIAG_SUBSYS_FS;
	buf[2] = FS_OP_WRITE & 0xFF;
	buf[3] = (FS_OP_WRITE >> 8) & 0xFF;
	buf[4] = fd & 0xFF;
	buf[5] = (fd >> 8) & 0xFF;
	buf[6] = dlen & 0xFF;
	buf[7] = (dlen >> 8) & 0xFF;
	memcpy(buf + 8, data, dlen);
	return 8 + dlen;
}

static size_t build_fs_delete(uint8_t *buf, size_t cap, const char *path)
{
	size_t plen = strlen(path) + 1;

	if (cap < 4 + 2 + plen)
		return 0;

	buf[0] = DIAG_CMD_DIAG_SUBSYS;
	buf[1] = DIAG_SUBSYS_FS;
	buf[2] = FS_OP_DELETE & 0xFF;
	buf[3] = (FS_OP_DELETE >> 8) & 0xFF;
	buf[4] = plen & 0xFF;
	buf[5] = (plen >> 8) & 0xFF;
	memcpy(buf + 6, path, plen);
	return 6 + plen;
}

/* ── Response parsing ─────────────────────────────────────────────── */

/* Remove trailing HDLC flag and CRC from raw read */
static int hdlc_decode_basic(uint8_t *dst, const uint8_t *src, size_t slen)
{
	size_t pos = 0;
	int esc = 0;

	for (size_t i = 0; i < slen && pos < 65536; i++) {
		if (src[i] == HDLC_FLAG)
			continue;
		if (src[i] == HDLC_ESC) {
			esc = 1;
			continue;
		}
		if (esc) {
			dst[pos++] = src[i] ^ HDLC_ESC_MASK;
			esc = 0;
		} else {
			dst[pos++] = src[i];
		}
	}
	/* Strip 2-byte CRC */
	if (pos >= 2)
		pos -= 2;
	else
		return -1;
	return (int)pos;
}

static void dump_rx(const uint8_t *raw, size_t raw_len)
{
	uint8_t dec[65536];
	int dlen;

	fprintf(stderr, "[%9.3f] RX raw %zu bytes:", mono_sec(), raw_len);
	for (size_t i = 0; i < raw_len && i < 32; i++)
		fprintf(stderr, " %02x", raw[i]);
	if (raw_len > 32)
		fprintf(stderr, " ...");
	fprintf(stderr, "\n");

	dlen = hdlc_decode_basic(dec, raw, raw_len);
	if (dlen > 0) {
		fprintf(stderr, "[%9.3f] RX dec %d bytes:", mono_sec(), dlen);
		for (int i = 0; i < dlen && i < 64; i++)
			fprintf(stderr, " %02x", dec[i]);
		if (dlen > 64)
			fprintf(stderr, " ...");
		fprintf(stderr, "\n");

		/* Print embedded strings */
		for (int i = 0; i < dlen; i++) {
			if (dec[i] >= 0x20 && dec[i] < 0x7f) {
				int start = i;
				while (i < dlen && dec[i] >= 0x20 && dec[i] < 0x7f) i++;
				if (i - start >= 4)
					fprintf(stderr, "  str@%d: \"%.*s\"\n",
						start, i - start, (const char *)&dec[start]);
			}
		}

		/* Check for BAD_CMD in decoded payload */
		if (dlen >= 1 && dec[0] == DIAG_BAD_CMD_F)
			fprintf(stderr, "[%9.3f] >>> BAD_CMD: modem rejected command 0x%02x\n",
				mono_sec(), dlen >= 2 ? dec[1] : 0);
		if (dlen >= 1 && dec[0] == DIAG_BAD_PARM_F)
			fprintf(stderr, "[%9.3f] >>> BAD_PARM\n", mono_sec());
	}
}

/* Check decoded response for BAD_CMD / BAD_PARM */
static int is_bad_response(const uint8_t *dec, int dlen)
{
	if (dlen < 1)
		return 0;
	return (dec[0] == DIAG_BAD_CMD_F || dec[0] == DIAG_BAD_PARM_F);
}

/* Get decoded response from raw HDLC frame, return length or -1 */
static int get_decoded_response(const uint8_t *rxbuf, size_t rxlen,
				uint8_t *dec, size_t dec_cap)
{
	int dlen = hdlc_decode_basic(dec, rxbuf, rxlen);
	if (dlen <= 0) {
		/* Maybe a raw response without HDLC flag? Use as-is */
		if (rxlen > 0) {
			size_t copy = rxlen < dec_cap ? rxlen : dec_cap;
			memcpy(dec, rxbuf, copy);
			return (int)copy;
		}
		return -1;
	}
	return dlen;
}

/* ── High-level operations ────────────────────────────────────────── */

static int fs_stat_path(int fd, const char *path,
			uint8_t *txbuf, size_t txcap,
			uint8_t *rxbuf, size_t rxcap)
{
	size_t plen = build_fs_stat(txbuf, txcap, path);
	if (!plen) {
		fprintf(stderr, "[%9.3f] STAT: build failed\n", mono_sec());
		return -1;
	}

	fprintf(stderr, "[%9.3f] FS_STAT(SUB) \"%s\" (%zu payload bytes)\n",
		mono_sec(), path, plen);
	if (send_frame(fd, txbuf, plen) < 0)
		return -1;

	int n = recv_response(fd, rxbuf, rxcap, 2000);
	if (n > 0) {
		dump_rx(rxbuf, (size_t)n);
		uint8_t dec[65536];
		int dlen = get_decoded_response(rxbuf, (size_t)n, dec, sizeof(dec));
		if (dlen > 0 && is_bad_response(dec, dlen))
			return -1;
	}
	return n;
}

static int fs_close_fd(int fd, uint16_t file_fd,
		       uint8_t *txbuf, size_t txcap,
		       uint8_t *rxbuf, size_t rxcap)
{
	size_t plen = build_fs_close(txbuf, txcap, file_fd);
	if (!plen)
		return -1;

	fprintf(stderr, "[%9.3f] FS_CLOSE(SUB) fd=%u\n", mono_sec(), file_fd);
	if (send_frame(fd, txbuf, plen) < 0)
		return -1;

	int n = recv_response(fd, rxbuf, rxcap, 2000);
	if (n > 0) {
		dump_rx(rxbuf, (size_t)n);
		uint8_t dec[65536];
		int dlen = get_decoded_response(rxbuf, (size_t)n, dec, sizeof(dec));
		if (dlen > 0 && !is_bad_response(dec, dlen))
			return 0;
	}
	return (n > 0) ? 0 : -1;
}

static int fs_write_path(int fd, const char *path, const uint8_t *data, size_t dlen,
			 uint8_t *txbuf, size_t txcap,
			 uint8_t *rxbuf, size_t rxcap)
{
	size_t plen;
	int n;
	uint16_t file_fd;

	/* 1. OPEN the file */
	plen = build_fs_open(txbuf, txcap, path,
			     FS_MODE_WRONLY | FS_MODE_CREATE | FS_MODE_TRUNC);
	if (!plen) {
		fprintf(stderr, "[%9.3f] OPEN: build failed\n", mono_sec());
		return -1;
	}

	fprintf(stderr, "[%9.3f] FS_OPEN(SUB) \"%s\" mode=0x%04x\n",
		mono_sec(), path, FS_MODE_WRONLY | FS_MODE_CREATE | FS_MODE_TRUNC);
	if (send_frame(fd, txbuf, plen) < 0)
		return -1;

	n = recv_response(fd, rxbuf, rxcap, 3000);
	if (n <= 0) {
		fprintf(stderr, "[%9.3f] OPEN: no response\n", mono_sec());
		return -1;
	}
	dump_rx(rxbuf, (size_t)n);

	/* Decode HDLC and check for BAD_CMD */
	{
		uint8_t dec[65536];
		int dlen = get_decoded_response(rxbuf, (size_t)n, dec, sizeof(dec));
		if (dlen > 0 && is_bad_response(dec, dlen))
			return -1;

		/* Successful FS_OPEN response (subsys): [4B][13][opL][opH][status][fdL][fdH] */
		if (dlen >= 7 && dec[0] == DIAG_CMD_DIAG_SUBSYS && dec[1] == DIAG_SUBSYS_FS) {
			file_fd = dec[5] | (dec[6] << 8);
			fprintf(stderr, "[%9.3f] OPEN response: status=%d fd=%u\n",
				mono_sec(), dec[4], file_fd);
			if (dec[4] != 0) {
				fprintf(stderr, "[%9.3f] OPEN: FS error status=%d\n",
					mono_sec(), dec[4]);
				return -1;
			}
		} else if (dlen >= 4) {
			/* Maybe direct FS_OP response: [59][op][status][fdL][fdH] */
			file_fd = dec[3] | (dec[4] << 8);
			fprintf(stderr, "[%9.3f] OPEN response (FS_OP): status=%d fd=%u\n",
				mono_sec(), dec[2], file_fd);
			if (dec[2] != 0) {
				fprintf(stderr, "[%9.3f] OPEN: FS error status=%d\n",
					mono_sec(), dec[2]);
				return -1;
			}
		} else {
			fprintf(stderr, "[%9.3f] OPEN: unexpected response (dlen=%d)\n",
				mono_sec(), dlen);
			return -1;
		}
	}

	/* 2. WRITE data */
	plen = build_fs_write(txbuf, txcap, file_fd, data, dlen);
	if (!plen) {
		fprintf(stderr, "[%9.3f] WRITE: build failed\n", mono_sec());
		fs_close_fd(fd, file_fd, txbuf, txcap, rxbuf, rxcap);
		return -1;
	}

	fprintf(stderr, "[%9.3f] FS_WRITE(SUB) fd=%u %zu bytes\n",
		mono_sec(), file_fd, dlen);
	if (send_frame(fd, txbuf, plen) < 0) {
		fs_close_fd(fd, file_fd, txbuf, txcap, rxbuf, rxcap);
		return -1;
	}

	n = recv_response(fd, rxbuf, rxcap, 3000);
	if (n > 0) {
		dump_rx(rxbuf, (size_t)n);
		uint8_t dec[65536];
		int dlen2 = get_decoded_response(rxbuf, (size_t)n, dec, sizeof(dec));
		if (dlen2 > 0 && is_bad_response(dec, dlen2)) {
			fs_close_fd(fd, file_fd, txbuf, txcap, rxbuf, rxcap);
			return -1;
		}
	}

	/* 3. CLOSE */
	return fs_close_fd(fd, file_fd, txbuf, txcap, rxbuf, rxcap);
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	const char *dev = "/dev/wwan0qcdm0";
	const char *path = NULL;
	const char *data_hex = NULL;
	int op = 0; /* 0=stat, 1=write, 2=delete */
	uint8_t txbuf[65536], rxbuf[65536], data_buf[4096];
	size_t data_len = 0;
	int fd, ret = 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--stat"))
			op = 0;
		else if (!strcmp(argv[i], "--write"))
			op = 1;
		else if (!strcmp(argv[i], "--delete"))
			op = 2;
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
				"usage: %s [--stat|--write|--delete] -p <efs_path> [-x <hex_data>] [-d <device>]\n"
				"  --stat    stat the path (default)\n"
				"  --write   write data to path (requires -x)\n"
				"  --delete  delete the path\n"
				"  -x <hex>  hex data for write (e.g. -x 01)\n"
				"  -d <dev>  DIAG device (default: /dev/wwan0qcdm0)\n",
				argv[0]);
			return 1;
		}
	}

	if (!path) {
		fprintf(stderr, "error: -p <path> required\n");
		return 1;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	fprintf(stderr, "[%9.3f] opened %s\n", mono_sec(), dev);

	switch (op) {
	case 0:
		ret = fs_stat_path(fd, path, txbuf, sizeof(txbuf),
				   rxbuf, sizeof(rxbuf));
		break;
	case 1:
		if (!data_len) {
			fprintf(stderr, "error: --write requires -x <hex>\n");
			break;
		}
		ret = fs_write_path(fd, path, data_buf, data_len,
				    txbuf, sizeof(txbuf),
				    rxbuf, sizeof(rxbuf));
		break;
	case 2:
		{
			size_t plen = build_fs_delete(txbuf, sizeof(txbuf), path);
			if (!plen) {
				fprintf(stderr, "DELETE: build failed\n");
				break;
			}
			fprintf(stderr, "[%9.3f] FS_DELETE \"%s\"\n", mono_sec(), path);
			if (send_frame(fd, txbuf, plen) == 0) {
				int n = recv_response(fd, rxbuf, sizeof(rxbuf), 2000);
				if (n > 0) dump_rx(rxbuf, (size_t)n);
				ret = (n > 0) ? 0 : -1;
			}
		}
		break;
	}

	close(fd);
	return ret;
}
