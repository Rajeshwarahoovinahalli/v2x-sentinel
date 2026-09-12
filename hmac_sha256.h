
#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define HMAC_SHA256_SIZE 32U

void hmac_sha256(const uint8_t *key, size_t key_len,
                  const uint8_t *data, size_t data_len,
                  uint8_t out[HMAC_SHA256_SIZE]);

int hmac_sha256_verify(const uint8_t a[HMAC_SHA256_SIZE], const uint8_t b[HMAC_SHA256_SIZE]);

#endif
