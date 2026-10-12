/* Host-side test vectors for the ring-3 TLS crypto (tls_sha256.c,
 * tls_chacha20.c, tls_x25519.c).  Independent references: NIST/RFC 4231
 * for SHA-256/HMAC, RFC 5869 for HKDF, an independently generated TLS 1.3
 * expand-label chain, RFC 8439 for ChaCha20-Poly1305 and RFC 7748 for
 * X25519.  Build and run: make test-crypto */

#include <stdio.h>
#include <string.h>

#include "tls_sha256.h"
#include "tls_chacha20.h"
#include "tls_x25519.h"

static int failures;
static int checks;

static void unhex(const char *s, unsigned char *out, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned int hi = (unsigned int)(s[i * 2]);
		unsigned int lo = (unsigned int)(s[i * 2 + 1]);

		hi = (hi <= '9') ? hi - '0' : ((hi | 0x20) - 'a' + 10);
		lo = (lo <= '9') ? lo - '0' : ((lo | 0x20) - 'a' + 10);
		out[i] = (unsigned char)((hi << 4) | lo);
	}
}

static void check(const char *name, const unsigned char *got, const char *exp_hex,
		  unsigned int n)
{
	unsigned char exp[128];
	unsigned int i;

	checks++;
	unhex(exp_hex, exp, n);
	if (memcmp(got, exp, n) == 0)
		return;
	failures++;
	printf("FAIL %s\n  got ", name);
	for (i = 0; i < n; i++)
		printf("%02x", got[i]);
	printf("\n  exp %s\n", exp_hex);
}

static void test_sha256(void)
{
	unsigned char out[32];
	static const unsigned char longmsg[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	sha256("", 0, out);
	check("sha256(empty)", out,
	      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	      32);
	sha256("abc", 3, out);
	check("sha256(abc)", out,
	      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      32);
	sha256(longmsg, sizeof(longmsg) - 1, out);
	check("sha256(448-bit)", out,
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
	      32);
}

static void test_hmac(void)
{
	unsigned char key[20], out[32];

	memset(key, 0x0b, sizeof(key));
	hmac_sha256(key, sizeof(key), "Hi There", 8, out);
	check("hmac rfc4231-1", out,
	      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
	      32);
	hmac_sha256((const unsigned char *)"Jefe", 4,
		    "what do ya want for nothing?", 28, out);
	check("hmac rfc4231-2", out,
	      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
	      32);
}

static void test_hkdf(void)
{
	unsigned char ikm[22], salt[13], info[10], prk[32], okm[42];
	unsigned char early[32], derived[32], hs[32], shared[32];
	unsigned char chs[32], key[32], iv[12], fin[32];
	unsigned char empty_hash[32], zero[32];
	static const char *chain[] = {
		"33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a",
		"6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba",
		"ddbe37614d014a8c19db0a47955ee6930b3ee727c408386ba274344962b0e015",
		"88af332c76d7e0dcb50151a802d39cd678dd53eb4a1b094024b5cb47128303f0",
		"b6b698fd98207b1c0edb367ab1d18cca98358cbe0989fc7de93a2dcf684c7fcd",
		"66adf518b563ca9d053f9cd2",
		"74c1f9cd877263ae11cadc6d24d7947c1eb7cc1e864828613f938250f9553684",
	};
	unsigned int i;

	memset(ikm, 0x0b, sizeof(ikm));
	unhex("000102030405060708090a0b0c", salt, sizeof(salt));
	unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));

	hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
	check("hkdf rfc5869 prk", prk,
	      "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
	      32);
	hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm));
	check("hkdf rfc5869 okm", okm,
	      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
	      "34007208d5b887185865", 42);

	/* TLS 1.3 key schedule, cross-checked against an independent
	 * implementation (the early/derived pair matches RFC 8448). */
	memset(zero, 0, sizeof(zero));
	sha256("", 0, empty_hash);
	hkdf_extract(0, 0, zero, 32, early);
	check("early secret", early, chain[0], 32);
	hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
	check("derive derived", derived, chain[1], 32);
	for (i = 0; i < 32; i++)
		shared[i] = (unsigned char)i;
	hkdf_extract(derived, 32, shared, 32, hs);
	check("handshake secret", hs, chain[2], 32);
	{
		unsigned char ctx[32];

		memset(ctx, 'X', sizeof(ctx));
		hkdf_expand_label(hs, "c hs traffic", ctx, 32, chs, 32);
	}
	check("c hs traffic", chs, chain[3], 32);
	hkdf_expand_label(chs, "key", 0, 0, key, 32);
	check("hs key", key, chain[4], 32);
	hkdf_expand_label(chs, "iv", 0, 0, iv, 12);
	check("hs iv", iv, chain[5], 12);
	hkdf_expand_label(chs, "finished", 0, 0, fin, 32);
	check("finished key", fin, chain[6], 32);
}

static void test_chacha(void)
{
	unsigned char key[32], nonce[12], ks[64], zero[64];
	unsigned char aad[12], pt[114], ct[114], tag[16], dec[114];
	static const char *aead_pt =
		"Ladies and Gentlemen of the class of '99: If I could offer you "
		"only one tip for the future, sunscreen would be it.";
	unsigned int i;

	for (i = 0; i < 32; i++)
		key[i] = (unsigned char)i;
	unhex("000000090000004a00000000", nonce, sizeof(nonce));
	memset(zero, 0, sizeof(zero));
	{
		struct chacha20_ctx c;

		chacha20_init(&c, key, nonce, 1);
		chacha20_xor(&c, ks, zero, sizeof(ks));
	}
	check("chacha20 block rfc8439", ks,
	      "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
	      "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
	      64);

	unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
	      key, 32);
	unhex("070000004041424344454647", nonce, sizeof(nonce));
	unhex("50515253c0c1c2c3c4c5c6c7", aad, sizeof(aad));
	memcpy(pt, aead_pt, sizeof(pt));

	aead_seal(key, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, tag);
	check("aead ct rfc8439", ct,
	      "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
	      "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
	      "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
	      "3ff4def08e4b7a9de576d26586cec64b6116", 114);
	check("aead tag rfc8439", tag,
	      "1ae10b594f09e26a7e902ecbd0600691", 16);

	memset(dec, 0, sizeof(dec));
	{
		struct aead_open_ctx a;
		unsigned char expect[16];
		unsigned int off = 0;

		aead_open_init(&a, key, nonce, aad, sizeof(aad));
		/* Odd chunk sizes exercise the incremental path. */
		while (off < sizeof(pt)) {
			unsigned int n = 7;

			if (n > sizeof(pt) - off)
				n = sizeof(pt) - off;
			aead_open_update(&a, dec + off, ct + off, n);
			off += n;
		}
		aead_open_tag(&a, expect);
		check("aead open tag", expect,
		      "1ae10b594f09e26a7e902ecbd0600691", 16);
	}
	check("aead open pt", dec, 
	      "4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
	      "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
	      "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
	      "637265656e20776f756c642062652069742e", 114);
}

static void test_x25519(void)
{
	unsigned char a_priv[32], a_pub[32], b_priv[32], b_pub[32];
	unsigned char shared[32];
	static const char *a_priv_hex =
		"77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
	static const char *b_priv_hex =
		"5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
	static const char *shared_hex =
		"4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

	unhex(a_priv_hex, a_priv, 32);
	unhex(b_priv_hex, b_priv, 32);
	x25519(a_pub, a_priv, x25519_base_point);
	check("x25519 alice pub", a_pub,
	      "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
	      32);
	x25519(b_pub, b_priv, x25519_base_point);
	check("x25519 bob pub", b_pub,
	      "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
	      32);
	x25519(shared, a_priv, b_pub);
	check("x25519 shared a", shared, shared_hex, 32);
	x25519(shared, b_priv, a_pub);
	check("x25519 shared b", shared, shared_hex, 32);
}

int main(void)
{
	test_sha256();
	test_hmac();
	test_hkdf();
	test_chacha();
	test_x25519();

	printf("%s: %d checks, %d failure(s)\n",
	       failures ? "FAIL" : "ok", checks, failures);
	return failures ? 1 : 0;
}
