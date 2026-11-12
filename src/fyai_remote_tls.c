/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include "fyai.h"
#include "fyai_cmd_int.h"
#include "fyai_host.h"
#include "fyai_remote_int.h"
#include "fyai_remote_tls.h"

#define FYAI_MODULE FYAIEM_REMOTE

#if OPENSSL_VERSION_NUMBER < 0x30000000L
#define SSL_get1_peer_certificate SSL_get_peer_certificate
#endif

/* The text of the last error of OpenSSL, which is then cleared. */
static const char *tls_error(char *buf, size_t size)
{
	unsigned long e = ERR_get_error();

	if (!e)
		return "no cause reported";
	ERR_error_string_n(e, buf, size);
	ERR_clear_error();
	return buf;
}

/* ---- the fingerprint ----------------------------------------------------- */

static int tls_spki_fingerprint(X509 *cert, char fp[FYAI_TLS_FP_SIZE])
{
	unsigned char *der = NULL, digest[EVP_MAX_MD_SIZE];
	unsigned int dlen = 0, i;
	int len;

	len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
	if (len <= 0)
		return -1;
	if (!EVP_Digest(der, (size_t)len, digest, &dlen, EVP_sha256(), NULL) ||
	    dlen != 32) {
		OPENSSL_free(der);
		return -1;
	}
	OPENSSL_free(der);
	memcpy(fp, "sha256:", 7);
	for (i = 0; i < dlen; i++)
		snprintf(fp + 7 + 2 * i, 3, "%02x", digest[i]);
	return 0;
}

/* @pin as the normal form "sha256:" and lowercase digits without colons. */
static int tls_pin_normal(const char *pin, char out[FYAI_TLS_FP_SIZE])
{
	size_t n = 0;

	if (!strncasecmp(pin, "sha256:", 7))
		pin += 7;
	memcpy(out, "sha256:", 7);
	for (; *pin; pin++) {
		if (*pin == ':')
			continue;
		if (!isxdigit((unsigned char)*pin) || n >= 64)
			return -1;
		out[7 + n++] = (char)tolower((unsigned char)*pin);
	}
	if (n != 64)
		return -1;
	out[7 + n] = '\0';
	return 0;
}

int fyai_tls_file_fingerprint(struct fyai_ctx *ctx, const char *path,
			      char fp[FYAI_TLS_FP_SIZE])
{
	char text[256];
	X509 *cert = NULL;
	FILE *file;
	int rc = -1, fp_rc;

	file = fopen(path, "re");
	fyai_error_check(ctx, file, out, "cannot open '%s': %s", path, strerror(errno));
	cert = PEM_read_X509(file, NULL, NULL, NULL);
	fclose(file);
	fyai_error_check(ctx, cert, out, "no certificate in '%s': %s", path,
			 tls_error(text, sizeof(text)));
	fp_rc = tls_spki_fingerprint(cert, fp);
	fyai_error_check(ctx, !fp_rc, out,
			 "cannot take the fingerprint of '%s'", path);
	rc = 0;
out:
	X509_free(cert);
	return rc;
}

/* ---- the key and the certificate of the server --------------------------- */

char *fyai_tls_default_path(struct fyai_ctx *ctx)
{
	const char *home = getenv("HOME");
	char *dir = NULL, *path = NULL;
	int rc;

	fyai_error_check(ctx, home && *home, out, "no HOME for the TLS directory");
	rc = asprintf(&dir, "%s/.fyai/tls", home);
	fyai_error_check(ctx, rc >= 0, out, "cannot make the TLS directory path");
	rc = fyai_remote_mkdirs(ctx->cfg, strlen(home), dir);
	fyai_error_check(ctx, !rc, out, "cannot make '%s'", dir);
	rc = asprintf(&path, "%s/server.pem", dir);
	if (rc < 0)
		path = NULL;
	fyai_error_check(ctx, path, out, "cannot make the path of the certificate");
out:
	free(dir);
	return path;
}

/* The subjectAltName value for @names and the names every server has. */
static char *tls_alt_names(struct fyai_ctx *ctx, const char *const *names)
{
	char host[256] = "", *text = NULL, *grown;
	const char *const base[] = { "localhost", "127.0.0.1", "::1", host, NULL };
	unsigned char addr[16];
	size_t len = 0, cap = 0, i, k;
	const char *name, *kind;
	char piece[300];
	size_t n;
	int pass;

	if (gethostname(host, sizeof(host) - 1))
		host[0] = '\0';
	for (pass = 0; pass < 2; pass++)
		for (i = 0; (name = pass ? (names ? names[i] : NULL) : base[i]); i++) {
			if (!*name)
				continue;
			for (k = 0; name[k]; k++)
				fyai_error_check(ctx, isalnum((unsigned char)name[k]) ||
						 strchr(".-:*_", name[k]), err,
						 "'%s' is not a name or an address", name);
			kind = inet_pton(AF_INET, name, addr) == 1 ||
			       inet_pton(AF_INET6, name, addr) == 1 ? "IP" : "DNS";
			n = (size_t)snprintf(piece, sizeof(piece), "%s%s:%s",
					     len ? "," : "", kind, name);
			fyai_error_check(ctx, n < sizeof(piece), err,
					 "the name '%s' is too long", name);
			if (len + n + 1 > cap) {
				cap = cap ? cap * 2 : 512;
				grown = realloc(text, cap);
				fyai_error_check(ctx, grown, err, "cannot keep the names");
				text = grown;
			}
			memcpy(text + len, piece, n + 1);
			len += n;
		}
	return text;
err:
	free(text);
	return NULL;
}

static int tls_add_ext(X509 *cert, int nid, const char *value)
{
	X509V3_CTX v3;
	X509_EXTENSION *ext;

	X509V3_set_ctx_nodb(&v3);
	X509V3_set_ctx(&v3, cert, cert, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &v3, nid, value);
	if (!ext)
		return -1;
	if (!X509_add_ext(cert, ext, -1)) {
		X509_EXTENSION_free(ext);
		return -1;
	}
	X509_EXTENSION_free(ext);
	return 0;
}

int fyai_tls_generate(struct fyai_ctx *ctx, const char *path,
		      const char *const *names)
{
	EVP_PKEY *key = NULL;
	EVP_PKEY_CTX *kctx = NULL;
	X509 *cert = NULL;
	X509_NAME *subject;
	char text[256], *alt = NULL, *tmp = NULL;
	unsigned char serial[8];
	ASN1_INTEGER *sn;
	uint64_t value;
	FILE *file = NULL;
	int fd = -1, rc = -1, ok, i;

	alt = tls_alt_names(ctx, names);
	if (!alt)
		goto out;
	kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
	ok = kctx && EVP_PKEY_keygen_init(kctx) > 0 &&
	     EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) > 0 &&
	     EVP_PKEY_keygen(kctx, &key) > 0;
	fyai_error_check(ctx, ok, out, "cannot make the key of the server: %s",
			 tls_error(text, sizeof(text)));
	cert = X509_new();
	fyai_error_check(ctx, cert, out, "cannot make the certificate");
	RAND_bytes(serial, sizeof(serial));
	serial[0] &= 0x7f;
	for (value = 0, i = 0; i < 8; i++)
		value = (value << 8) | serial[i];
	sn = X509_get_serialNumber(cert);
	ok = X509_set_version(cert, 2) && ASN1_INTEGER_set_uint64(sn, value | 1) &&
	     X509_gmtime_adj(X509_getm_notBefore(cert), -86400) &&
	     X509_gmtime_adj(X509_getm_notAfter(cert), 3650L * 86400L) &&
	     X509_set_pubkey(cert, key);
	subject = X509_get_subject_name(cert);
	ok = ok && X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
					      (const unsigned char *)"fyai serve",
					      -1, -1, 0) &&
	     X509_set_issuer_name(cert, subject);
	ok = ok && !tls_add_ext(cert, NID_basic_constraints, "critical,CA:FALSE") &&
	     !tls_add_ext(cert, NID_key_usage, "critical,digitalSignature") &&
	     !tls_add_ext(cert, NID_ext_key_usage, "serverAuth") &&
	     !tls_add_ext(cert, NID_subject_alt_name, alt) &&
	     X509_sign(cert, key, EVP_sha256());
	fyai_error_check(ctx, ok, out, "cannot make the certificate: %s",
			 tls_error(text, sizeof(text)));
	/* A file that nobody reads half written: write aside, then rename. */
	i = asprintf(&tmp, "%s.XXXXXX", path);
	fyai_error_check(ctx, i >= 0, out, "cannot make the temporary path");
	fd = mkstemp(tmp);
	fyai_error_check(ctx, fd >= 0, out, "cannot make '%s': %s", tmp, strerror(errno));
	file = fdopen(fd, "w");
	fyai_error_check(ctx, file, out, "cannot write '%s'", tmp);
	fd = -1;
	ok = PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL) &&
	     PEM_write_X509(file, cert) && !fflush(file) && !fsync(fileno(file));
	fyai_error_check(ctx, ok, out, "cannot write '%s': %s", tmp,
			 tls_error(text, sizeof(text)));
	ok = !fclose(file);
	file = NULL;
	fyai_error_check(ctx, ok, out, "cannot write '%s': %s", tmp, strerror(errno));
	ok = !rename(tmp, path);
	fyai_error_check(ctx, ok, out, "cannot replace '%s': %s", path, strerror(errno));
	free(tmp);
	tmp = NULL;
	rc = 0;
out:
	if (file)
		fclose(file);
	if (fd >= 0)
		close(fd);
	if (tmp) {
		unlink(tmp);
		free(tmp);
	}
	X509_free(cert);
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(kctx);
	free(alt);
	return rc;
}

/* Choose http/1.1 when the client names a protocol; none is not an error. */
static int tls_alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen,
		    const unsigned char *in, unsigned int inlen, void *arg)
{
	static const unsigned char http11[] = "\x08http/1.1";

	(void)ssl;
	(void)arg;
	if (SSL_select_next_proto((unsigned char **)(uintptr_t)out, outlen, http11,
				  sizeof(http11) - 1, in, inlen) ==
	    OPENSSL_NPN_NEGOTIATED)
		return SSL_TLSEXT_ERR_OK;
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* True when only the owner can read @path, a regular file. */
static bool tls_private_file(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISREG(st.st_mode) && !(st.st_mode & 077);
}

SSL_CTX *fyai_tls_server_ctx(struct fyai_ctx *ctx, const char *cert,
			     const char *key, const char *const *names,
			     char fp[FYAI_TLS_FP_SIZE])
{
	SSL_CTX *tls = NULL;
	char text[256], *generated = NULL;
	const char *cert_file = cert, *key_file = key;
	struct stat st;
	int ok;

	fyai_error_check(ctx, !!cert == !!key, err, "--tls-cert and --tls-key "
			 "come together");
	if (!cert) {
		generated = fyai_tls_default_path(ctx);
		if (!generated)
			goto err;
		if (stat(generated, &st) && errno == ENOENT &&
		    fyai_tls_generate(ctx, generated, names))
			goto err;
		cert_file = key_file = generated;
	}
	fyai_error_check(ctx, tls_private_file(key_file), err, "the key file '%s' "
			 "must be a file that only you can read", key_file);
	tls = SSL_CTX_new(TLS_server_method());
	fyai_error_check(ctx, tls, err, "cannot make the TLS context: %s",
			 tls_error(text, sizeof(text)));
	SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION);
	SSL_CTX_set_options(tls, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
			    SSL_OP_CIPHER_SERVER_PREFERENCE);
	SSL_CTX_set_mode(tls, SSL_MODE_ENABLE_PARTIAL_WRITE |
			 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS);
	SSL_CTX_set_alpn_select_cb(tls, tls_alpn, NULL);
	ok = SSL_CTX_use_certificate_chain_file(tls, cert_file) == 1 &&
	     SSL_CTX_use_PrivateKey_file(tls, key_file, SSL_FILETYPE_PEM) == 1 &&
	     SSL_CTX_check_private_key(tls) == 1;
	fyai_error_check(ctx, ok, err, "cannot use the certificate '%s' and the "
			 "key '%s': %s", cert_file, key_file,
			 tls_error(text, sizeof(text)));
	if (fyai_tls_file_fingerprint(ctx, cert_file, fp))
		goto err;
	free(generated);
	return tls;
err:
	SSL_CTX_free(tls);
	free(generated);
	return NULL;
}

/* ---- the client ----------------------------------------------------------- */

/* The fingerprint of the key that the server showed, for the diagnostic. */
static char tls_seen[FYAI_TLS_FP_SIZE];

static int tls_verify_cb(int ok, X509_STORE_CTX *store)
{
	X509 *cert;

	if (X509_STORE_CTX_get_error_depth(store) == 0) {
		cert = X509_STORE_CTX_get_current_cert(store);
		if (cert && tls_spki_fingerprint(cert, tls_seen))
			tls_seen[0] = '\0';
	}
	return ok;
}

SSL *fyai_tls_client_new(struct fyai_ctx *ctx, const char *host, const char *pin,
			 const char *ca_file)
{
	static const unsigned char alpn[] = "\x08http/1.1";
	char text[256], want[FYAI_TLS_FP_SIZE];
	unsigned char addr[16];
	SSL_CTX *tls = NULL;
	SSL *ssl = NULL, *made = NULL;
	int ok, pin_rc = 0;
	bool is_ip;

	tls_seen[0] = '\0';
	if (pin)
		pin_rc = tls_pin_normal(pin, want);
	fyai_error_check(ctx, !pin_rc, out,
			 "the pin must be sha256: and 64 hexadecimal digits");
	tls = SSL_CTX_new(TLS_client_method());
	fyai_error_check(ctx, tls, out, "cannot make the TLS context: %s",
			 tls_error(text, sizeof(text)));
	SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION);
	SSL_CTX_set_options(tls, SSL_OP_NO_COMPRESSION);
	if (pin) {
		/* The pin is the whole check, as the key of a host in ssh. */
		SSL_CTX_set_verify(tls, SSL_VERIFY_NONE, NULL);
	} else {
		SSL_CTX_set_verify(tls, SSL_VERIFY_PEER, tls_verify_cb);
		ok = ca_file && *ca_file ?
			SSL_CTX_load_verify_locations(tls, ca_file, NULL) :
			SSL_CTX_set_default_verify_paths(tls);
		fyai_error_check(ctx, ok == 1, out, "cannot load the trusted "
				 "certificates%s%s: %s", ca_file ? " of " : "",
				 ca_file ? ca_file : "", tls_error(text, sizeof(text)));
	}
	ssl = SSL_new(tls);
	fyai_error_check(ctx, ssl, out, "cannot make the TLS connection");
	is_ip = inet_pton(AF_INET, host, addr) == 1 ||
		inet_pton(AF_INET6, host, addr) == 1;
	if (!pin) {
		ok = is_ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) :
			     SSL_set1_host(ssl, host);
		fyai_error_check(ctx, ok == 1, out, "cannot set the host to check");
	}
	if (!is_ip)
		SSL_set_tlsext_host_name(ssl, host);
	SSL_set_alpn_protos(ssl, alpn, sizeof(alpn) - 1);
	made = ssl;
	ssl = NULL;
out:
	SSL_free(ssl);
	SSL_CTX_free(tls);
	return made;
}

void fyai_tls_client_error(struct fyai_ctx *ctx, SSL *ssl, const char *host,
			   const char *pin)
{
	char text[256];
	long verify = SSL_get_verify_result(ssl);

	if (verify != X509_V_OK && !pin) {
		fyai_error(ctx, "the certificate of '%s' is not trusted: %s%s%s%s",
			   host, X509_verify_cert_error_string(verify),
			   tls_seen[0] ? "; its key is " : "", tls_seen,
			   tls_seen[0] ? ": to trust it, set FYAI_REMOTE_PIN to that, "
			   "or FYAI_REMOTE_CA to the file of its CA" : "");
	} else {
		fyai_error(ctx, "the TLS handshake with '%s' failed: %s", host,
			   tls_error(text, sizeof(text)));
	}
}

int fyai_tls_client_verify(struct fyai_ctx *ctx, SSL *ssl, const char *host,
			   const char *pin)
{
	char want[FYAI_TLS_FP_SIZE], got[FYAI_TLS_FP_SIZE];
	X509 *peer;
	int rc;

	if (!pin)
		return 0;
	rc = tls_pin_normal(pin, want);
	fyai_error_check(ctx, !rc, err, "the pin must be sha256: and 64 "
			 "hexadecimal digits");
	peer = SSL_get1_peer_certificate(ssl);
	fyai_error_check(ctx, peer, err, "the server showed no certificate");
	rc = tls_spki_fingerprint(peer, got);
	X509_free(peer);
	fyai_error_check(ctx, !rc, err, "the server showed no certificate");
	if (CRYPTO_memcmp(got, want, FYAI_TLS_FP_SIZE - 1)) {
		fyai_error(ctx, "the key of '%s' is %s, not the pinned %s", host,
			   got, want);
		goto err;
	}
	return 0;
err:
	return -1;
}

int fyai_tls_connect(struct fyai_ctx *ctx, int fd, const char *host,
		     const char *pin, const char *ca_file, SSL **sslp)
{
	SSL *ssl;
	int ok;

	ssl = fyai_tls_client_new(ctx, host, pin, ca_file);
	if (!ssl)
		goto err;
	SSL_set_fd(ssl, fd);
	ok = SSL_connect(ssl);
	if (ok != 1) {
		fyai_tls_client_error(ctx, ssl, host, pin);
		goto err_free;
	}
	if (fyai_tls_client_verify(ctx, ssl, host, pin))
		goto err_free;
	*sslp = ssl;
	return 0;
err_free:
	SSL_free(ssl);
err:
	return -1;
}

/* ---- the commands --------------------------------------------------------- */

int fyai_cmd_remote_fingerprint(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	char fp[FYAI_TLS_FP_SIZE], *path = NULL;
	const char *file = fyai_cmd_arg_str(call, "file");
	struct stat st;
	int rc = -1;

	if (!file) {
		path = fyai_tls_default_path(ctx);
		if (!path)
			return -1;
		file = path;
		fyai_error_check(ctx, !stat(file, &st), out, "there is no key at '%s': "
				 "start `fyai serve --listen https://ADDRESS`, or `fyai "
				 "remote rekey`", file);
	}
	if (fyai_tls_file_fingerprint(ctx, file, fp))
		goto out;
	*result = fy_mapping(call->gb, "fingerprint", fp, "file", file);
	rc = 0;
out:
	free(path);
	return rc;
}

int fyai_cmd_remote_rekey(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic list = fy_get(call->args, "tls_name", fy_invalid);
	char fp[FYAI_TLS_FP_SIZE], *path;
	const char **names = NULL, *name;
	size_t n = 0;
	int rc = -1;

	path = fyai_tls_default_path(ctx);
	if (!path)
		return -1;
	names = calloc((fy_is_valid(list) ? fy_len(list) : 0) + 1, sizeof(*names));
	fyai_error_check(ctx, names, out, "cannot keep the TLS names");
	if (fy_is_valid(list))
		fy_foreach(name, list)
			names[n++] = name;
	if (fyai_tls_generate(ctx, path, names) ||
	    fyai_tls_file_fingerprint(ctx, path, fp))
		goto out;
	*result = fy_mapping(call->gb, "fingerprint", fp, "file", path);
	rc = 0;
out:
	free(names);
	free(path);
	return rc;
}
