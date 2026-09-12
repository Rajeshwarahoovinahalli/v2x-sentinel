
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCK_SIZE  64U
#define SHA256_DIGEST_SIZE 32U

typedef struct {
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);

void sha256(const uint8_t *data, size_t len, uint8_t hash[SHA256_DIGEST_SIZE]);

#endif
