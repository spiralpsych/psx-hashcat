#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t bytes[32];
} Sha256Digest;

Sha256Digest sha256_compute(const void *data, size_t length);
int sha256_equal(const Sha256Digest *a, const Sha256Digest *b);
