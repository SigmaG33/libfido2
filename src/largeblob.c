/*
 * Copyright (c) 2020 Yubico AB. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <openssl/sha.h>

#include "fido.h"
#include "fido/es256.h"

#define LARGEBLOB_DIGEST_LENGTH	16
#define LARGEBLOB_NONCE_LENGTH	12
#define LARGEBLOB_TAG_LENGTH	16

typedef struct largeblob {
	size_t      plaintext_len;
	fido_blob_t ciphertext;
	fido_blob_t nonce;
} largeblob_t;

static largeblob_t *
largeblob_new(void)
{
	return calloc(1, sizeof(largeblob_t));
}

static void
largeblob_reset(largeblob_t *blob)
{
	fido_blob_reset(&blob->ciphertext);
	fido_blob_reset(&blob->nonce);
	blob->plaintext_len = 0;
}

static void
largeblob_free(largeblob_t **blob_ptr)
{
	largeblob_t *blob;

	if (blob_ptr == NULL || (blob = *blob_ptr) == NULL)
		return;
	largeblob_reset(blob);
	free(blob);
	*blob_ptr = NULL;
}

static fido_blob_t *
largeblob_aad(uint64_t plaintext_len)
{
	uint8_t buf[4 + sizeof(uint64_t)];
	fido_blob_t *aad;

	buf[0] = 0x62; /* b */
	buf[1] = 0x6c; /* l */
	buf[2] = 0x6f; /* o */
	buf[3] = 0x62; /* b */
	plaintext_len = htole64(plaintext_len);
	memcpy(&buf[4], &plaintext_len, sizeof(uint64_t));
	if ((aad = fido_blob_new()) == NULL ||
	    fido_blob_set(aad, buf, sizeof(buf)) < 0)
		fido_blob_free(&aad);

	return aad;
}

static fido_blob_t *
largeblob_decrypt(const largeblob_t *blob, const fido_blob_t *key)
{
	fido_blob_t *plaintext, *aad = NULL;

	if ((plaintext = fido_blob_new()) == NULL ||
	    (aad = largeblob_aad(blob->plaintext_len)) == NULL ||
	    aes256_gcm_dec(key, &blob->nonce, aad, &blob->ciphertext,
	    plaintext) < 0)
		fido_blob_free(&plaintext);

	fido_blob_free(&aad);

	return plaintext;
}

static int
largeblob_get_nonce(largeblob_t *blob)
{
	uint8_t buf[LARGEBLOB_NONCE_LENGTH];
	int ok = -1;

	if (fido_get_random(buf, sizeof(buf)) < 0 ||
	    fido_blob_set(&blob->nonce, buf, sizeof(buf)) < 0)
		goto fail;
	ok = 0;
fail:
	explicit_bzero(buf, sizeof(buf));

	return ok;
}

static int
largeblob_seal(largeblob_t *blob, const fido_blob_t *plaintext,
    const fido_blob_t *key)
{
	fido_blob_t *pt, *aad = NULL;
	int ok = -1;

	if ((pt = fido_blob_new()) == NULL)
		return -1;
	if (fido_compress(pt, plaintext) != FIDO_OK) {
		fido_log_debug("%s: fido_compress", __func__);
		goto fail;
	}
	if ((aad = largeblob_aad(plaintext->len)) == NULL) {
		fido_log_debug("%s: largeblob_add", __func__);
		goto fail;
	}
	if (largeblob_get_nonce(blob) < 0) {
		fido_log_debug("%s: largeblob_get_nonce", __func__);
		goto fail;
	}
	if (aes256_gcm_enc(key, &blob->nonce, aad, pt, &blob->ciphertext) < 0) {
		fido_log_debug("%s: aes256_gcm_enc", __func__);
		goto fail;
	}
	blob->plaintext_len = plaintext->len;

	ok = 0;
fail:
	fido_blob_free(&pt);
	fido_blob_free(&aad);

	return ok;
}

static size_t
max_fragment_length(fido_dev_t *dev)
{
	uint64_t	maxfraglen;

	maxfraglen = fido_dev_maxmsgsize(dev);
	if (maxfraglen > SIZE_MAX)
		maxfraglen = SIZE_MAX;
	if (maxfraglen > FIDO_MAXMSG)
		maxfraglen = FIDO_MAXMSG;

	maxfraglen = maxfraglen > 64 ? maxfraglen - 64 : 0;

	return (size_t)maxfraglen;
}

static int
largeblob_get_tx(fido_dev_t *dev, size_t offset, size_t count)
{
	fido_blob_t f;
	cbor_item_t *argv[3];
	int r;

	memset(argv, 0, sizeof(argv));
	memset(&f, 0, sizeof(f));

	if ((argv[0] = cbor_build_uint(count)) == NULL ||
	    (argv[2] = cbor_build_uint(offset)) == NULL) {
		fido_log_debug("%s: cbor encode", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}
	if (cbor_build_frame(CTAP_CBOR_LARGEBLOB, argv, nitems(argv), &f) < 0 ||
	    fido_tx(dev, CTAP_CMD_CBOR, f.ptr, f.len) < 0) {
		fido_log_debug("%s: fido_tx", __func__);
		r = FIDO_ERR_TX;
		goto fail;
	}

	r = FIDO_OK;
fail:
	cbor_vector_free(argv, nitems(argv));
	free(f.ptr);

	return r;
}

static int
parse_largeblob_reply(const cbor_item_t *key, const cbor_item_t *val,
    void *arg)
{
	if (cbor_isa_uint(key) == false ||
	    cbor_int_get_width(key) != CBOR_INT_8 ||
	    cbor_get_uint8(key) != 1) {
		fido_log_debug("%s: cbor type", __func__);
		return 0; /* ignore */
	}

	return fido_blob_decode(val, arg);
}

static int
largeblob_get_rx(fido_dev_t *dev, fido_blob_t **chunk, int ms)
{
	unsigned char reply[FIDO_MAXMSG];
	int reply_len, r;

	if ((reply_len = fido_rx(dev, CTAP_CMD_CBOR, &reply, sizeof(reply),
	    ms)) < 0) {
		fido_log_debug("%s: fido_rx", __func__);
		return FIDO_ERR_RX;
	}
	if ((*chunk = fido_blob_new()) == NULL) {
		fido_log_debug("%s: fido_blob_new", __func__);
		return FIDO_ERR_INTERNAL;
	}
	if ((r = cbor_parse_reply(reply, (size_t)reply_len, *chunk,
	    parse_largeblob_reply)) != FIDO_OK) {
		fido_log_debug("%s: parse_largeblob_reply", __func__);
		fido_blob_free(chunk);
		return r;
	}

	return FIDO_OK;
}

static cbor_item_t *
largeblob_array_load(const uint8_t *ptr, size_t len)
{
	struct cbor_load_result cbor;
	cbor_item_t *item;

	if (len < SHA256_DIGEST_LENGTH) {
		fido_log_debug("%s: len", __func__);
		return NULL;
	}
	len -= SHA256_DIGEST_LENGTH;
	if ((item = cbor_load(ptr, len, &cbor)) == NULL) {
		fido_log_debug("%s: cbor_load", __func__);
		return NULL;
	}
	if (!cbor_isa_array(item) || !cbor_array_is_definite(item)) {
		fido_log_debug("%s: cbor type", __func__);
		cbor_decref(&item);
		return NULL;
	}

	return item;
}

static size_t
get_chunklen(fido_dev_t *dev)
{
	uint64_t maxchunklen;

	if ((maxchunklen = fido_dev_maxmsgsize(dev)) > SIZE_MAX)
		maxchunklen = SIZE_MAX;
	if (maxchunklen > FIDO_MAXMSG)
		maxchunklen = FIDO_MAXMSG;
	maxchunklen = maxchunklen > 64 ? maxchunklen - 64 : 0;

	return (size_t)maxchunklen;
}

static int
largeblob_do_decode(const cbor_item_t *key, const cbor_item_t *val, void *arg)
{
	largeblob_t *blob = arg;
	uint64_t origsize;

	if (cbor_isa_uint(key) == false ||
	    cbor_int_get_width(key) != CBOR_INT_8) {
		fido_log_debug("%s: cbor type", __func__);
		return 0; /* ignore */
	}

	switch (cbor_get_uint8(key)) {
	case 1: /* ciphertext */
		if (fido_blob_decode(val, &blob->ciphertext) < 0 ||
		    blob->ciphertext.len < LARGEBLOB_TAG_LENGTH)
			return -1;
		return 0;
	case 2: /* nonce */
		if (fido_blob_decode(val, &blob->nonce) < 0 ||
		    blob->nonce.len != LARGEBLOB_NONCE_LENGTH)
			return -1;
		return 0;
	case 3: /* origSize */
		if (!cbor_isa_uint(val) ||
		    (origsize = cbor_get_int(val)) > SIZE_MAX)
			return -1;
		blob->plaintext_len = (size_t)origsize;
		return 0;
	default: /* ignore */
		fido_log_debug("%s: cbor type", __func__);
		return 0;
	}
}

static int
largeblob_decode(largeblob_t *blob, const cbor_item_t *item)
{
	if (!cbor_isa_map(item) || !cbor_map_is_definite(item)) {
		fido_log_debug("%s: cbor type", __func__);
		return -1;
	}
	if (cbor_map_iter(item, blob, largeblob_do_decode) < 0) {
		fido_log_debug("%s: cbor_map_iter", __func__);
		return -1;
	}
	if (fido_blob_is_empty(&blob->ciphertext) ||
	    fido_blob_is_empty(&blob->nonce) || blob->plaintext_len == 0) {
		fido_log_debug("%s: incomplete blob", __func__);
		return -1;
	}

	return 0;
}

static cbor_item_t *
largeblob_encode(const fido_blob_t *plaintext, const fido_blob_t *key)
{
	largeblob_t *blob;
	cbor_item_t *argv[3], *item = NULL;

	memset(argv, 0, sizeof(argv));
	if ((blob = largeblob_new()) == NULL ||
	    largeblob_seal(blob, plaintext, key) < 0) {
		fido_log_debug("%s: largeblob_seal", __func__);
		goto fail;
	}
	if ((argv[0] = fido_blob_encode(&blob->ciphertext)) == NULL ||
	    (argv[1] = fido_blob_encode(&blob->nonce)) == NULL ||
	    (argv[2] = cbor_build_uint(blob->plaintext_len)) == NULL) {
		fido_log_debug("%s: cbor encode", __func__);
		goto fail;
	}
	item = cbor_flatten_vector(argv, nitems(argv));
fail:
	cbor_vector_free(argv, nitems(argv));
	largeblob_free(&blob);

	return item;
}

static int
largeblob_array_lookup(fido_blob_t *out, size_t *idx, const cbor_item_t *item,
    const fido_blob_t *key)
{
	cbor_item_t **v;
	fido_blob_t *plaintext = NULL;
	largeblob_t blob;
	int r;

	memset(&blob, 0, sizeof(blob));

	if ((v = cbor_array_handle(item)) == NULL)
		return FIDO_ERR_INVALID_ARGUMENT;
	for (size_t i = 0; i < cbor_array_size(item); i++) {
		if (largeblob_decode(&blob, v[i]) < 0)
			fido_log_debug("%s: largeblob_decode", __func__);
		else if ((plaintext = largeblob_decrypt(&blob, key)) == NULL) {
			fido_log_debug("%s: largeblob_decrypt", __func__);
			largeblob_reset(&blob);
		} else {
			if (idx != NULL)
				*idx = i;
			break;
		}
	}
	if (plaintext == NULL) {
		fido_log_debug("%s: not found", __func__);
		return FIDO_ERR_NOTFOUND;
	}
	if (out != NULL)
		r = fido_uncompress(out, plaintext, blob.plaintext_len);
	else
		r = FIDO_OK;

	fido_blob_free(&plaintext);
	largeblob_reset(&blob);

	return r;
}

static int
largeblob_array_insert(cbor_item_t **array, const fido_blob_t *key,
    cbor_item_t *blob)
{
	size_t idx;
	int r;

	switch (r = largeblob_array_lookup(NULL, &idx, *array, key)) {
	case FIDO_OK:
		if (!cbor_array_replace(*array, idx, blob)) {
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}
		break;
	case FIDO_ERR_NOTFOUND:
		if (cbor_array_append(array, blob) < 0) {
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}
		break;
	default:
		fido_log_debug("%s: largeblob_array_lookup", __func__);
		goto fail;
	}

	r = FIDO_OK;
fail:
	return r;
}

static int
largeblob_array_remove(cbor_item_t **array, const fido_blob_t *key)
{
	size_t idx;
	int r;

	if ((r = largeblob_array_lookup(NULL, &idx, *array, key)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_array_lookup", __func__);
		goto fail;
	}
	if (cbor_array_drop(array, idx) < 0) {
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	r = FIDO_OK;
fail:
	return r;
}

static int
largeblob_array_digest(u_char out[LARGEBLOB_DIGEST_LENGTH], const u_char *data,
    size_t len)
{
	u_char dgst[SHA256_DIGEST_LENGTH];

	if (data == NULL || len == 0 || SHA256(data, len, dgst) != dgst)
		return -1;
	memcpy(out, dgst, LARGEBLOB_DIGEST_LENGTH);

	return 0;
}

static int
largeblob_array_check(const fido_blob_t *array)
{
	u_char expected_hash[LARGEBLOB_DIGEST_LENGTH];
	size_t body_len;

	fido_log_xxd(array->ptr, array->len, __func__);
	if (array->len < sizeof(expected_hash)) {
		fido_log_debug("%s: len %zu", __func__, array->len);
		return -1;
	}
	body_len = array->len - sizeof(expected_hash);
	if (largeblob_array_digest(expected_hash, array->ptr, body_len) < 0) {
		fido_log_debug("%s: largeblob_array_digest", __func__);
		return -1;
	}

	return timingsafe_bcmp(expected_hash, array->ptr + body_len,
	    sizeof(expected_hash));
}

static int
largeblob_get_array(fido_dev_t *dev, cbor_item_t **item)
{
	fido_blob_t *array, *chunk = NULL;
	size_t n;
	int r;

	if ((n = get_chunklen(dev)) == 0)
		return FIDO_ERR_INVALID_ARGUMENT;
	if ((array = fido_blob_new()) == NULL)
		return FIDO_ERR_INTERNAL;
	do {
		if ((r = largeblob_get_tx(dev, array->len, n)) != FIDO_OK ||
		    (r = largeblob_get_rx(dev, &chunk, -1)) != FIDO_OK) {
			fido_log_debug("%s: largeblob_get_wait %zu/%zu",
			    __func__, array->len, n);
			goto fail;
		}
		if (fido_blob_append(array, chunk->ptr, chunk->len) < 0) {
			fido_log_debug("%s: fido_blob_append", __func__);
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}
		fido_blob_free(&chunk);
	} while (chunk->len == n);

	if (largeblob_array_check(array) != 0)
		*item = cbor_new_definite_array(0); /* per spec */
	else
		*item = largeblob_array_load(array->ptr, array->len);
	if (*item == NULL)
		r = FIDO_ERR_INTERNAL;
	else
		r = FIDO_OK;
fail:
	fido_blob_free(&array);
	fido_blob_free(&chunk);

	return r;
}

static int
prepare_hmac(size_t offset, const u_char *data, size_t len, fido_blob_t *hmac)
{
	uint8_t buf[32 + 2 + sizeof(uint32_t) + SHA256_DIGEST_LENGTH];
	uint32_t u32_offset;

	if (data == NULL || len == 0 || offset > UINT32_MAX) {
		fido_log_debug("%s: offset=%zu", __func__, offset);
		return -1;
	}
	memset(buf, 0xff, 32);
	buf[32] = CTAP_CBOR_LARGEBLOB;
	buf[33] = 0x00;
	u32_offset = htole32((uint32_t)offset);
	memcpy(&buf[34], &u32_offset, sizeof(uint32_t));
	if (SHA256(data, len, &buf[36]) != &buf[36]) {
		fido_log_debug("%s: SHA256", __func__);
		return -1;
	}

	return fido_blob_set(hmac, buf, sizeof(buf));
}

static int
largeblob_set_tx(fido_dev_t *dev, const fido_blob_t *token, const u_char *chunk,
    size_t chunk_len, size_t offset, size_t totalsiz)
{
	fido_blob_t *hmac = NULL, f;
	cbor_item_t *argv[6];
	int r;

	memset(argv, 0, sizeof(argv));
	memset(&f, 0, sizeof(f));

	if ((argv[1] = cbor_build_bytestring(chunk, chunk_len)) == NULL ||
	    (argv[2] = cbor_build_uint(offset)) == NULL ||
	    (offset == 0 && (argv[3] = cbor_build_uint(totalsiz)) == NULL)) {
		fido_log_debug("%s: cbor encode", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}
	if (token != NULL) {
		if ((hmac = fido_blob_new()) == NULL ||
		    prepare_hmac(offset, chunk, chunk_len, hmac) < 0 ||
		    (argv[4] = cbor_encode_pin_auth(dev, token, hmac)) == NULL ||
		    (argv[5] = cbor_encode_pin_opt(dev)) == NULL) {
			fido_log_debug("%s: cbor_encode_pin_auth", __func__);
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}
	}
	if (cbor_build_frame(CTAP_CBOR_LARGEBLOB, argv, nitems(argv), &f) < 0 ||
	    fido_tx(dev, CTAP_CMD_CBOR, f.ptr, f.len) < 0) {
		fido_log_debug("%s: fido_tx", __func__);
		r = FIDO_ERR_TX;
		goto fail;
	}

	r = FIDO_OK;
fail:
	cbor_vector_free(argv, nitems(argv));
	fido_blob_free(&hmac);
	free(f.ptr);

	return r;
}

static int
largeblob_array_set_wait(fido_dev_t *dev, const cbor_item_t *arr,
    const char *pin, int ms)
{
	unsigned char	 dgst[SHA256_DIGEST_LENGTH];
	fido_blob_t	*token = NULL;
	fido_blob_t	*ecdh = NULL;
	es256_pk_t	*pk = NULL;
	unsigned char	*cbor = NULL;
	size_t		 cbor_len;
	size_t		 cbor_alloc_len;
	size_t		 offset = 0;
	size_t		 maxlen = 0;
	SHA256_CTX	 ctx;
	int		 r;

	if ((maxlen = max_fragment_length(dev)) == 0) {
		fido_log_debug("%s: maxlen=%zu", __func__, maxlen);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((cbor_len = cbor_serialize_alloc(arr, &cbor, &cbor_alloc_len)) == 0 ||
	    cbor_len > (SIZE_MAX - LARGEBLOB_DIGEST_LENGTH)) {
		fido_log_debug("%s: cbor_serialize_alloc", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if (fido_dev_can_get_uv_token(dev, pin, FIDO_OPT_OMIT)) {
		if ((token = fido_blob_new()) == NULL) {
			fido_log_debug("%s: fido_blob_new", __func__);
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}

		if ((r = fido_do_ecdh(dev, &pk, &ecdh)) != FIDO_OK ||
		    (r = fido_dev_get_uv_token(dev, CTAP_CBOR_LARGEBLOB, pin, ecdh, pk,
		    NULL, token)) != FIDO_OK) {
			fido_log_debug("%s: fido_dev_get_uv_token", __func__);
			goto fail;
		}
	}

	if (SHA256_Init(&ctx) == 0) {
		fido_log_debug("%s: SHA256_Init", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	while (offset < cbor_len) {
		size_t len = maxlen < cbor_len - offset ?
		    maxlen : cbor_len - offset;
		if (SHA256_Update(&ctx, cbor + offset, len) == 0) {
			fido_log_debug("%s: SHA256_Update", __func__);
			r = FIDO_ERR_INTERNAL;
			goto fail;
		}

		if ((r = largeblob_set_tx(dev, token, cbor + offset, len,
		    offset, cbor_len + LARGEBLOB_DIGEST_LENGTH)) != FIDO_OK ||
		    (r = fido_rx_cbor_status(dev, ms)) != FIDO_OK) {
			fido_log_debug("%s: largeblob_set_tx 1",
			    __func__);
			goto fail;
		}

		offset += len;
	}

	if (SHA256_Final(dgst, &ctx) == 0) {
		fido_log_debug("%s: SHA256_Final", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((r = largeblob_set_tx(dev, token, dgst,
	    LARGEBLOB_DIGEST_LENGTH, offset,
	    cbor_len + LARGEBLOB_DIGEST_LENGTH)) != FIDO_OK ||
	    (r = fido_rx_cbor_status(dev, ms)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_set_tx 2", __func__);
		goto fail;
	}

	r = FIDO_OK;

fail:
	fido_blob_free(&token);
	fido_blob_free(&ecdh);
	es256_pk_free(&pk);
	free(cbor);

	return r;
}

int
fido_dev_largeblob_get(fido_dev_t *dev, const unsigned char *key_ptr,
    size_t key_len, fido_blob_t *blob)
{
	fido_blob_t	*key = NULL;
	cbor_item_t	*array = NULL;
	int		 r;

	if (blob == NULL || key_ptr == NULL || key_len != 32) {
		fido_log_debug("%s: blob=%p, key_ptr=%p, key_len=%zu",
		    __func__, (void *)blob, (const void *)key_ptr, key_len);
		r = FIDO_ERR_INVALID_ARGUMENT;
		goto fail;
	}

	fido_blob_reset(blob);

	if ((key = fido_blob_new()) == NULL ||
	     fido_blob_set(key, key_ptr, key_len) < 0) {
		fido_log_debug("%s: fido_blob_set", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((r = largeblob_get_array(dev, &array)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_get_array", __func__);
		goto fail;
	}

	if ((r = largeblob_array_lookup(blob, NULL, array, key)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_array_lookup", __func__);
		goto fail;
	}

fail:
	fido_blob_free(&key);

	if (array != NULL)
		cbor_decref(&array);

	return r;
}

int
fido_dev_largeblob_put(fido_dev_t *dev, const unsigned char *key_ptr,
    size_t key_len, const fido_blob_t *blob, const char *pin)
{
	cbor_item_t	*array = NULL;
	cbor_item_t	*item = NULL;
	fido_blob_t	*key = NULL;
	int		 r;

	if (blob == NULL || fido_blob_is_empty(blob) ||
	    key_ptr == NULL || key_len != 32) {
		fido_log_debug("%s: blob=%p, key_ptr=%p, key_len=%zu",
		    __func__, (const void *)blob, (const void *)key_ptr, key_len);
		r = FIDO_ERR_INVALID_ARGUMENT;
		goto fail;
	}

	if ((key = fido_blob_new()) == NULL ||
	    fido_blob_set(key, key_ptr, key_len) < 0) {
		fido_log_debug("%s: fido_blob_new", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((item = largeblob_encode(blob, key)) == NULL ||
	    largeblob_get_array(dev, &array) != FIDO_OK) {
		fido_log_debug("%s: largeblob_get_array", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((r = largeblob_array_insert(&array, key, item)) != FIDO_OK ||
	    (r = largeblob_array_set_wait(dev, array, pin, -1)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_array_set_wait", __func__);
		goto fail;
	}

	r = FIDO_OK;
fail:
	fido_blob_free(&key);
	if (array != NULL)
		cbor_decref(&array);
	if (item != NULL)
		cbor_decref(&item);

	return r;
}

int
fido_dev_largeblob_remove(fido_dev_t *dev, const unsigned char *key_ptr,
    size_t key_len, const char *pin)
{
	cbor_item_t	*array = NULL;
	fido_blob_t	*key = NULL;
	int		 r;

	if (key_ptr == NULL || key_len != 32) {
		fido_log_debug("%s: key_ptr = %p, key_len = %zu",
			__func__, (const void *)key_ptr, key_len);
		r = FIDO_ERR_INVALID_ARGUMENT;
		goto fail;
	}

	if ((key = fido_blob_new()) == NULL ||
	    fido_blob_set(key, key_ptr, key_len) < 0) {
		fido_log_debug("%s: fido_blob_set", __func__);
		r = FIDO_ERR_INTERNAL;
		goto fail;
	}

	if ((r = largeblob_get_array(dev, &array)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_get_array", __func__);
		goto fail;
	}

	if ((r = largeblob_array_remove(&array, key)) != FIDO_OK ||
	    (r = largeblob_array_set_wait(dev, array, pin, -1)) != FIDO_OK) {
		fido_log_debug("%s: largeblob_array_set_wait", __func__);
		goto fail;
	}

	r = FIDO_OK;
fail:
	fido_blob_free(&key);
	if (array != NULL)
		cbor_decref(&array);

	return r;
}
