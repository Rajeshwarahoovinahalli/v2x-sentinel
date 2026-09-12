
#include "hmac_sha256.h"
#include "sha256.h"
#include <string.h>

void hmac_sha256(const uint8_t *key, size_t key_len,
                  const uint8_t *data, size_t data_len,
                  uint8_t out[HMAC_SHA256_SIZE])
{
    uint8_t k_ipad[SHA256_BLOCK_SIZE];
    uint8_t k_opad[SHA256_BLOCK_SIZE];
    uint8_t key_block[SHA256_BLOCK_SIZE];
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    SHA256_CTX ctx;

    memset(key_block, 0, SHA256_BLOCK_SIZE);

    if (key_len > SHA256_BLOCK_SIZE)
    {

        sha256(key, key_len, key_block);
    }
    else
    {
        memcpy(key_block, key, key_len);
    }

    for (uint32_t i = 0U; i < SHA256_BLOCK_SIZE; i++)
    {
        k_ipad[i] = key_block[i] ^ 0x36U;
        k_opad[i] = key_block[i] ^ 0x5cU;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner_hash, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, out);
}

int hmac_sha256_verify(const uint8_t a[HMAC_SHA256_SIZE], const uint8_t b[HMAC_SHA256_SIZE])
{
    uint8_t diff = 0U;
    for (uint32_t i = 0U; i < HMAC_SHA256_SIZE; i++)
    {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (diff == 0U) ? 1 : 0;
}
