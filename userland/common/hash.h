/* hash.h — MD5, SHA-1 and SHA-2 for the crypto APIs (advapi32, bcrypt) */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { NOVA_MD5, NOVA_SHA1, NOVA_SHA256, NOVA_SHA384, NOVA_SHA512 };

typedef struct {
    int alg;
    uint64_t len;                   /* bytes hashed so far */
    union { uint32_t s32[8]; uint64_t s64[8]; } h;
    uint8_t buf[128];
    unsigned fill;
} NovaHash;

size_t nova_hash_size(int alg);         /* digest bytes */
size_t nova_hash_block(int alg);        /* block bytes (for HMAC) */
void   nova_hash_init(NovaHash *c, int alg);
void   nova_hash_update(NovaHash *c, const void *data, size_t n);
void   nova_hash_final(NovaHash *c, uint8_t *out);

/* HMAC: init with the key, update with the message, final */
typedef struct { NovaHash inner, outer; } NovaHmac;
void nova_hmac_init(NovaHmac *m, int alg, const void *key, size_t klen);
void nova_hmac_update(NovaHmac *m, const void *data, size_t n);
void nova_hmac_final(NovaHmac *m, uint8_t *out);
