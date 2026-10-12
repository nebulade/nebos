/* Minimal TLS 1.3 client (RFC 8446) over the kernel TCP client:
 * X25519 key exchange, TLS_CHACHA20_POLY1305_SHA256, no certificate
 * verification (the leaf SHA-256 is printed for manual comparison).
 *
 * The record layer is a small state machine; incoming records are
 * decrypted in chunks straight from the TCP stream (SYS_TCP_RECV) so a
 * 16 KB record never needs more than one plaintext buffer.  Handshake
 * messages are fed byte-wise through a transcript-hashing parser, so a
 * large Certificate message is hashed as it flows past.
 *
 * Phase A trade-off: this authenticates nothing.  See tls.h. */

#include "ucmd_lib.h"
#include "tls.h"
#include "tls_sha256.h"
#include "tls_chacha20.h"
#include "tls_x25519.h"
#include "tls_rng.h"

#define TLS_RAW_SIZE       2048
#define TLS_REC_PT_MAX     (16384 + 256)
#define TLS_SH_MAX         1024
#define TLS_TX_MAX         2048
#define TLS_APP_CHUNK      1024
#define TLS_HS_DEADLINE_MS 15000

/* ---- state (one connection at a time) ---- */

static unsigned char raw_buf[TLS_RAW_SIZE];
static unsigned int raw_len, raw_pos;

static unsigned char rec_pt[TLS_REC_PT_MAX];   /* decrypted record payload */
static unsigned int app_off, app_len;          /* pending app data in rec_pt */

static unsigned char sh_buf[TLS_SH_MAX];
static unsigned char tx_buf[TLS_TX_MAX];

static unsigned int peer_mss = 536;
static int phase;                              /* 0 clear, 1 hs keys, 2 app keys */
static unsigned long long rx_seq, tx_seq;
static unsigned char c_hs[32], s_hs[32], c_ap[32], s_ap[32];
static unsigned char c_hs_key[32], s_hs_key[32], c_hs_iv[12], s_hs_iv[12];
static unsigned char c_ap_key[32], s_ap_key[32], c_ap_iv[12], s_ap_iv[12];
static unsigned char master_secret[32];
static unsigned char x_priv[32];

static struct sha256_ctx transcript;
static unsigned char fin_data[32];
static int eof, err;

/* handshake message parser */
static unsigned int hm_type, hm_len, hm_got;

/* Certificate streaming parser */
static unsigned int cert_stage;        /* 0 ctx-len, 1 ctx, 2 list-len, 3 cert-len, 4 cert data, 5 ext-len, 6 ext data, 9 done */
static unsigned int cert_ctx_left, cert_hdr_got, cert_list_left;
static unsigned int cert_cur_left, cert_seen;
static unsigned int cert_ext_got, cert_ext_left;
static struct sha256_ctx cert_hash;

static const unsigned char zeros32[32];
static const unsigned char sha256_empty[32] = {
	0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
	0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
	0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
	0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};

/* ---- small helpers ---- */

static int eq32(const unsigned char *a, const unsigned char *b)
{
	unsigned int i, d = 0;

	for (i = 0; i < 32; i++)
		d |= a[i] ^ b[i];
	return d == 0;
}

static void transcript_hash(unsigned char out[32])
{
	struct sha256_ctx t;

	sha256_copy(&t, &transcript);
	sha256_final(&t, out);
}

static void derive_secret_th(const unsigned char *secret, const char *label,
			     const unsigned char th[32], unsigned char out[32])
{
	hkdf_expand_label(secret, label, th, 32, out, 32);
}

static void derive_keys(const unsigned char *secret, unsigned char key[32],
			unsigned char iv[12])
{
	hkdf_expand_label(secret, "key", 0, 0, key, 32);
	hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}

static void nonce_for(unsigned char nonce[12], const unsigned char iv[12],
		      unsigned long long seq)
{
	unsigned int i;

	for (i = 0; i < 12; i++)
		nonce[i] = iv[i];
	for (i = 0; i < 8; i++)
		nonce[11 - i] ^= (unsigned char)(seq >> (8 * i));
}

/* ---- raw TCP input ---- */

static int raw_fill(unsigned int timeout)
{
	int r = (int)u_syscall3(SYS_TCP_RECV, (int)raw_buf,
				(int)sizeof(raw_buf), (int)timeout);

	if (r > 0) {
		raw_len = (unsigned int)r;
		raw_pos = 0;
		return 0;
	}
	if (r == 0) {
		eof = 1;
		return -1;
	}
	err = (r == -2) ? 1 : 2;
	return -1;
}

static int raw_read(void *dst, unsigned int n, unsigned int deadline)
{
	unsigned char *d = dst;

	while (n) {
		unsigned int avail = raw_len - raw_pos;
		unsigned int take, i;

		if (!avail) {
			int left;

			if (err || eof)
				return -1;
			left = (int)(deadline - u_ticks_ms());
			if (left <= 0) {
				err = 2;
				return -1;
			}
			if (raw_fill((unsigned int)left) != 0)
				return -1;
			continue;
		}
		take = avail < n ? avail : n;
		for (i = 0; i < take; i++)
			d[i] = raw_buf[raw_pos + i];
		raw_pos += take;
		d += take;
		n -= take;
	}
	return 0;
}

/* ---- record output ---- */

static int tcp_send_all(const unsigned char *p, unsigned int n)
{
	while (n) {
		unsigned int take = n < peer_mss ? n : peer_mss;

		if (u_syscall2(SYS_TCP_SEND, (int)p, (int)take) != 0)
			return -1;
		p += take;
		n -= take;
	}
	return 0;
}

static int send_plain(unsigned char type, const unsigned char *data,
		      unsigned int len)
{
	unsigned char hdr[5];

	hdr[0] = type;
	hdr[1] = 3;
	hdr[2] = 3;
	hdr[3] = (unsigned char)(len >> 8);
	hdr[4] = (unsigned char)len;
	if (tcp_send_all(hdr, 5) != 0)
		return -1;
	return tcp_send_all(data, len);
}

static int send_record(unsigned char inner, const unsigned char *data,
		       unsigned int len, int use_hs)
{
	const unsigned char *key = use_hs ? c_hs_key : c_ap_key;
	const unsigned char *iv = use_hs ? c_hs_iv : c_ap_iv;
	unsigned char nonce[12], aad[5], tag[16];
	unsigned int plen = len + 1, rl = plen + 16, i;

	if (5 + rl > TLS_TX_MAX)
		return -1;
	for (i = 0; i < len; i++)
		tx_buf[5 + i] = data[i];
	tx_buf[5 + len] = inner;

	aad[0] = 23;
	aad[1] = 3;
	aad[2] = 3;
	aad[3] = (unsigned char)(rl >> 8);
	aad[4] = (unsigned char)rl;
	for (i = 0; i < 5; i++)
		tx_buf[i] = aad[i];          /* the record header on the wire */
	nonce_for(nonce, iv, tx_seq);
	aead_seal(key, nonce, aad, 5, tx_buf + 5, plen, tx_buf + 5, tag);
	for (i = 0; i < 16; i++)
		tx_buf[5 + plen + i] = tag[i];
	tx_seq++;

	return tcp_send_all(tx_buf, 5 + rl);
}

/* ---- handshake: ServerHello ---- */

static void sh_parse(unsigned int len)
{
	const unsigned char *p = sh_buf;
	unsigned char shared[32], early[32], d1[32], d2[32], hs[32], th[32];
	unsigned char server_key[32];
	unsigned int o, sid, cipher, elen, i, j;
	int have_key = 0, have_ver = 0;

	if (err)
		return;
	if (len < 2 + 32 + 1 + 2 + 1 + 2) {
		err = 3;
		return;
	}
	o = 2 + 32;
	sid = p[o++];
	if (o + sid + 3 > len) {
		err = 3;
		return;
	}
	o += sid;
	cipher = ((unsigned int)p[o] << 8) | p[o + 1];
	o += 2;
	if (cipher != 0x1303) {
		u_puts("tls: server chose another cipher suite\n");
		err = 4;
		return;
	}
	o++;                                   /* legacy_compression_method */
	elen = ((unsigned int)p[o] << 8) | p[o + 1];
	o += 2;
	if (o + elen != len) {
		err = 3;
		return;
	}
	for (i = 0; i + 4 <= elen; ) {
		unsigned int et = ((unsigned int)p[o + i] << 8) | p[o + i + 1];
		unsigned int el = ((unsigned int)p[o + i + 2] << 8) |
				  p[o + i + 3];
		const unsigned char *d = p + o + i + 4;

		if (i + 4 + el > elen) {
			err = 3;
			return;
		}
		if (et == 0x0033 && el == 36 && d[0] == 0x00 && d[1] == 0x1d &&
		    d[2] == 0x00 && d[3] == 32) {
			for (j = 0; j < 32; j++)
				server_key[j] = d[4 + j];
			have_key = 1;
		}
		if (et == 0x002b && el == 2 && d[0] == 3 && d[1] == 4)
			have_ver = 1;
		i += 4 + el;
	}
	if (!have_ver) {
		u_puts("tls: server did not agree on TLS 1.3\n");
		err = 4;
		return;
	}
	if (!have_key) {
		u_puts("tls: server sent no X25519 key share\n");
		err = 4;
		return;
	}

	x25519(shared, x_priv, server_key);
	hkdf_extract(0, 0, zeros32, 32, early);
	derive_secret_th(early, "derived", sha256_empty, d1);
	hkdf_extract(d1, 32, shared, 32, hs);
	transcript_hash(th);
	derive_secret_th(hs, "c hs traffic", th, c_hs);
	derive_secret_th(hs, "s hs traffic", th, s_hs);
	derive_keys(c_hs, c_hs_key, c_hs_iv);
	derive_keys(s_hs, s_hs_key, s_hs_iv);
	derive_secret_th(hs, "derived", sha256_empty, d2);
	hkdf_extract(d2, 32, zeros32, 32, master_secret);

	phase = 1;
	u_puts("tls: server hello, TLS_CHACHA20_POLY1305_SHA256 + X25519\n");
}

/* ---- handshake: Certificate (streamed, fingerprint only) ---- */

static unsigned char cert_lenbuf[3];

static void cert_msg_done(void)
{
	unsigned char fp[32];
	unsigned int i;

	if (cert_stage != 9 || cert_seen == 0) {
		err = 3;
		return;
	}
	sha256_final(&cert_hash, fp);
	u_puts("tls: cert[0] sha256=");
	for (i = 0; i < 32; i++)
		u_put_hex2(fp[i]);
	u_putc('\n');
	u_puts("tls: chain has ");
	u_putdec(cert_seen);
	u_puts(" certificate(s), NOT verified\n");
}

static void cert_byte(unsigned char b)
{
	switch (cert_stage) {
	case 0:
		sha256_init(&cert_hash);
		cert_ctx_left = b;
		cert_stage = cert_ctx_left ? 1 : 2;
		break;
	case 1:
		if (--cert_ctx_left == 0)
			cert_stage = 2;
		break;
	case 2:
		cert_lenbuf[cert_hdr_got++] = b;
		if (cert_hdr_got == 3) {
			cert_hdr_got = 0;
			cert_list_left = ((unsigned int)cert_lenbuf[0] << 16) |
					 ((unsigned int)cert_lenbuf[1] << 8) |
					 cert_lenbuf[2];
			cert_stage = cert_list_left ? 3 : 9;
		}
		break;
	case 3:
		cert_lenbuf[cert_hdr_got++] = b;
		cert_list_left--;
		if (cert_hdr_got == 3) {
			cert_hdr_got = 0;
			cert_cur_left = ((unsigned int)cert_lenbuf[0] << 16) |
					((unsigned int)cert_lenbuf[1] << 8) |
					cert_lenbuf[2];
			if (cert_cur_left == 0) {
				cert_seen++;
				cert_ext_got = 0;
				cert_stage = 5;
			} else {
				cert_stage = 4;
			}
		}
		break;
	case 4:
		if (cert_seen == 0)
			sha256_update(&cert_hash, &b, 1);
		cert_cur_left--;
		cert_list_left--;
		if (cert_cur_left == 0) {
			cert_seen++;
			cert_ext_got = 0;
			cert_stage = 5;
		}
		break;
	case 5:                                  /* per-entry extensions */
		cert_lenbuf[cert_ext_got++] = b;
		cert_list_left--;
		if (cert_ext_got == 2) {
			cert_ext_left = ((unsigned int)cert_lenbuf[0] << 8) |
					cert_lenbuf[1];
			cert_stage = cert_ext_left ? 6 :
				     (cert_list_left ? 3 : 9);
		}
		break;
	case 6:
		cert_ext_left--;
		cert_list_left--;
		if (cert_ext_left == 0)
			cert_stage = cert_list_left ? 3 : 9;
		break;
	default:
		break;
	}
}

/* ---- handshake message parser ---- */

static void hs_msg_complete(void);

static void hs_byte(unsigned char b)
{
	unsigned int body_pos;

	if (hm_got < 4) {
		if (hm_got == 0) {
			hm_type = b;
			hm_len = 0;
		} else {
			hm_len = (hm_len << 8) | b;
		}
		if (hm_type != 20)
			sha256_update(&transcript, &b, 1);
		hm_got++;
		if (hm_got == 4 && hm_len == 0)
			hs_msg_complete();
		return;
	}
	if (hm_got - 4 >= hm_len) {             /* defensive */
		hs_msg_complete();
		return;
	}
	body_pos = hm_got - 4;
	if (hm_type != 20)
		sha256_update(&transcript, &b, 1);

	switch (hm_type) {
	case 2:
		if (body_pos < TLS_SH_MAX)
			sh_buf[body_pos] = b;
		else
			err = 3;
		break;
	case 11:
		cert_byte(b);
		break;
	case 15:
		break;                          /* CertificateVerify: not checked */
	case 20:
		if (body_pos < 32)
			fin_data[body_pos] = b;
		break;
	default:
		break;                          /* EE and post-handshake: skip */
	}

	hm_got++;
	if (hm_got - 4 == hm_len)
		hs_msg_complete();
}

static void hs_finish(unsigned int len);

static void hs_msg_complete(void)
{
	unsigned int type = hm_type, len = hm_len;

	hm_got = 0;
	switch (type) {
	case 2:
		sh_parse(len);
		break;
	case 11:
		cert_msg_done();
		break;
	case 20:
		hs_finish(len);
		break;
	default:
		break;
	}
}

static void hs_finish(unsigned int len)
{
	unsigned char th[32], fk[32], expect[32], msg[36];

	if (err)
		return;
	if (len != 32 || phase != 1) {
		err = 3;
		return;
	}

	transcript_hash(th);
	hkdf_expand_label(s_hs, "finished", 0, 0, fk, 32);
	hmac_sha256(fk, 32, th, 32, expect);
	if (!eq32(expect, fin_data)) {
		u_puts("tls: bad server Finished (handshake mismatch)\n");
		err = 5;
		return;
	}

	/* Transcript += server Finished. */
	msg[0] = 20;
	msg[1] = 0;
	msg[2] = 0;
	msg[3] = 32;
	sha256_update(&transcript, msg, 4);
	sha256_update(&transcript, fin_data, 32);

	/* Application traffic secrets (context: CH .. server Finished). */
	transcript_hash(th);
	derive_secret_th(master_secret, "c ap traffic", th, c_ap);
	derive_secret_th(master_secret, "s ap traffic", th, s_ap);
	derive_keys(c_ap, c_ap_key, c_ap_iv);
	derive_keys(s_ap, s_ap_key, s_ap_iv);

	/* Our Finished, under the handshake keys. */
	hkdf_expand_label(c_hs, "finished", 0, 0, fk, 32);
	hmac_sha256(fk, 32, th, 32, expect);
	msg[0] = 20;
	msg[1] = 0;
	msg[2] = 0;
	msg[3] = 32;
	{
		unsigned int i;

		for (i = 0; i < 32; i++)
			msg[4 + i] = expect[i];
	}
	if (send_record(22, msg, 36, 1) != 0) {
		err = 1;
		return;
	}

	phase = 2;
	rx_seq = 0;
	tx_seq = 0;
}

/* ---- record input ---- */

static void inner_process(unsigned int len)
{
	unsigned int i = len;
	unsigned char type;

	while (i > 0 && rec_pt[i - 1] == 0)
		i--;
	if (i == 0) {
		err = 3;
		return;
	}
	type = rec_pt[i - 1];
	len = i - 1;

	switch (type) {
	case 22:
		for (i = 0; i < len && !err; i++)
			hs_byte(rec_pt[i]);
		break;
	case 23:
		app_off = 0;
		app_len = len;
		break;
	case 21:
		if (len >= 2) {
			if (rec_pt[1] == 0) {
				eof = 1;        /* close_notify */
			} else {
				u_puts("tls: alert ");
				u_putdec(rec_pt[1]);
				u_putc('\n');
				err = 6;
			}
		}
		break;
	default:
		break;                          /* inner CCS etc. */
	}
}

static int pump_record(unsigned int deadline)
{
	unsigned char hdr[5], tag[16], expect[16];
	unsigned int len, i;

	if (err || eof)
		return -1;
	if (raw_read(hdr, 5, deadline) != 0)
		return -1;
	len = ((unsigned int)hdr[3] << 8) | hdr[4];

	if (hdr[0] == 20) {                     /* CCS (middlebox mode) */
		if (len > 4 || raw_read(rec_pt, len, deadline) != 0) {
			err = 3;
			return -1;
		}
		return 0;
	}
	if (hdr[0] == 21) {                     /* plaintext alert */
		if (len != 2 || raw_read(rec_pt, 2, deadline) != 0) {
			err = 3;
			return -1;
		}
		u_puts("tls: server alert: ");
		switch (rec_pt[1]) {
		case 40:
			u_puts("handshake failure");
			break;
		case 47:
			u_puts("illegal parameter");
			break;
		case 70:
			u_puts("protocol version (the server may not support TLS 1.3)");
			break;
		case 80:
			u_puts("internal error");
			break;
		default:
			u_puts("description ");
			u_putdec(rec_pt[1]);
			break;
		}
		u_puts(" (level ");
		u_putdec(rec_pt[0]);
		u_puts(")\n");
		err = 4;
		return -1;
	}
	if (phase == 0) {
		if (hdr[0] != 22 || len > TLS_SH_MAX) {
			err = 3;
			return -1;
		}
		if (raw_read(sh_buf, len, deadline) != 0)
			return -1;
		for (i = 0; i < len && !err; i++)
			hs_byte(sh_buf[i]);
		return 0;
	}
	if (hdr[0] != 23 || len < 17 || len > TLS_REC_PT_MAX) {
		err = 3;
		return -1;
	}
	{
		const unsigned char *key = (phase == 1) ? s_hs_key : s_ap_key;
		const unsigned char *iv = (phase == 1) ? s_hs_iv : s_ap_iv;
		struct aead_open_ctx a;
		unsigned char nonce[12];
		unsigned int ptlen = len - 16, off = 0;

		nonce_for(nonce, iv, rx_seq);
		aead_open_init(&a, key, nonce, hdr, 5);
		while (off < ptlen) {
			unsigned int n = ptlen - off;

			if (n > 1024)
				n = 1024;
			if (raw_read(rec_pt + off, n, deadline) != 0)
				return -1;
			aead_open_update(&a, rec_pt + off, rec_pt + off, n);
			off += n;
		}
		if (raw_read(tag, 16, deadline) != 0)
			return -1;
		aead_open_tag(&a, expect);
		for (i = 0; i < 16; i++) {
			if (tag[i] != expect[i]) {
				u_puts("tls: record authentication failed\n");
				err = 5;
				return -1;
			}
		}
		rx_seq++;
		inner_process(ptlen);
	}
	return 0;
}

/* ---- ClientHello ---- */

static unsigned char ch_buf[512];
static unsigned int ch_n;
static int ch_over;

static void ch_u8(unsigned int v)
{
	if (ch_n < sizeof(ch_buf))
		ch_buf[ch_n++] = (unsigned char)v;
	else
		ch_over = 1;
}

static void ch_u16(unsigned int v)
{
	ch_u8(v >> 8);
	ch_u8(v);
}

static void ch_u24(unsigned int v)
{
	ch_u8(v >> 16);
	ch_u8(v >> 8);
	ch_u8(v);
}

static void ch_bytes(const void *p, unsigned int n)
{
	const unsigned char *b = p;

	while (n--)
		ch_u8(*b++);
}

static int build_client_hello(const char *sni)
{
	unsigned char cr[32], sid[32], pub[32];
	unsigned int hostlen = 0, ext_pos, ext_start;

	while (sni[hostlen])
		hostlen++;
	if (hostlen == 0 || hostlen > 200)
		return -1;

	ch_n = 0;
	ch_over = 0;

	ch_u8(1);                              /* client_hello */
	ch_u24(0);                             /* length (patched below) */
	ch_u16(0x0303);                        /* legacy_version */
	tls_random(cr, 32);
	ch_bytes(cr, 32);
	tls_random(sid, 32);
	ch_u8(32);                             /* legacy_session_id */
	ch_bytes(sid, 32);
	ch_u16(2);                             /* cipher suites */
	ch_u16(0x1303);
	ch_u8(1);                              /* compression */
	ch_u8(0);

	ext_pos = ch_n;
	ch_u16(0);                             /* extensions length (patched) */
	ext_start = ch_n;

	/* supported_versions */
	ch_u16(0x002b);
	ch_u16(3);
	ch_u8(2);
	ch_u16(0x0304);
	/* supported_groups */
	ch_u16(0x000a);
	ch_u16(4);
	ch_u16(2);
	ch_u16(0x001d);
	/* signature_algorithms (unused in phase A, syntactically required) */
	ch_u16(0x000d);
	ch_u16(6);
	ch_u16(4);
	ch_u16(0x0804);
	ch_u16(0x0403);
	/* key_share */
	tls_random(x_priv, 32);
	x25519(pub, x_priv, x25519_base_point);
	ch_u16(0x0033);
	ch_u16(38);
	ch_u16(36);
	ch_u16(0x001d);
	ch_u16(32);
	ch_bytes(pub, 32);
	/* server_name */
	ch_u16(0x0000);
	ch_u16(2 + 1 + 2 + hostlen);
	ch_u16(1 + 2 + hostlen);
	ch_u8(0);
	ch_u16(hostlen);
	ch_bytes(sni, hostlen);
	/* application_layer_protocol_negotiation: http/1.1 */
	ch_u16(0x0010);
	ch_u16(2 + 1 + 8);
	ch_u16(1 + 8);
	ch_u8(8);
	ch_bytes("http/1.1", 8);

	if (ch_over)
		return -1;
	ch_buf[ext_pos] = (unsigned char)((ch_n - ext_start) >> 8);
	ch_buf[ext_pos + 1] = (unsigned char)(ch_n - ext_start);
	ch_buf[1] = (unsigned char)((ch_n - 4) >> 16);
	ch_buf[2] = (unsigned char)((ch_n - 4) >> 8);
	ch_buf[3] = (unsigned char)(ch_n - 4);
	return (int)ch_n;
}

/* ---- public API ---- */

int tls_connect(const char *sni)
{
	unsigned int n;
	unsigned int deadline;
	int r;

	phase = 0;
	rx_seq = tx_seq = 0;
	raw_len = raw_pos = 0;
	app_off = app_len = 0;
	eof = err = 0;
	hm_got = 0;
	cert_stage = cert_seen = 0;
	cert_hdr_got = 0;

	peer_mss = (unsigned int)u_syscall0(SYS_TCP_PEER_MSS);
	if (peer_mss < 64 || peer_mss > 1460)
		peer_mss = 536;

	sha256_init(&transcript);
	r = build_client_hello(sni);
	if (r <= 0) {
		u_puts("tls: bad host name\n");
		return -1;
	}
	n = (unsigned int)r;
	sha256_update(&transcript, ch_buf, n);

	if (send_plain(22, ch_buf, n) != 0) {
		u_puts("tls: send failed\n");
		return -1;
	}
	{
		static const unsigned char ccs[1] = { 1 };

		send_plain(20, ccs, 1);        /* middlebox compatibility */
	}
	u_puts("tls: client hello sent (");
	u_putdec(n);
	u_puts(" bytes)\n");

	deadline = u_ticks_ms() + TLS_HS_DEADLINE_MS;
	while (phase != 2 && !err && !eof) {
		if (pump_record(deadline) != 0)
			break;
	}
	if (phase != 2) {
		u_puts("tls: handshake failed");
		if (err == 4)
			u_puts(" (server does not support TLS 1.3)");
		else if (err == 2)
			u_puts(" (timeout)");
		else if (err == 1)
			u_puts(" (connection reset)");
		else if (eof)
			u_puts(" (connection closed)");
		u_putc('\n');
		return -1;
	}
	u_puts("tls: handshake done\n");
	return 0;
}

int tls_send(const void *buf, unsigned int len)
{
	if (phase != 2 || len == 0 || len > TLS_APP_CHUNK)
		return -1;
	return send_record(23, buf, len, 0);
}

int tls_recv(void *buf, unsigned int max, unsigned int timeout_ms)
{
	unsigned char *out = buf;
	unsigned int deadline = u_ticks_ms() + timeout_ms;

	for (;;) {
		unsigned int i, n;

		if (app_len) {
			n = app_len - app_off;
			if (n > max)
				n = max;
			for (i = 0; i < n; i++)
				out[i] = rec_pt[app_off + i];
			app_off += n;
			if (app_off == app_len)
				app_len = 0;
			return (int)n;
		}
		if (eof)
			return 0;
		if (err)
			return (err == 2) ? -1 : -2;
		if ((int)(u_ticks_ms() - deadline) >= 0)
			return -1;
		if (pump_record(deadline) != 0) {
			if (eof)
				return 0;
			return (err == 2) ? -1 : -2;
		}
	}
}

void tls_close(void)
{
	static const unsigned char cn[2] = { 1, 0 };

	if (phase == 2 && !err && !eof)
		send_record(21, cn, 2, 0);
}
