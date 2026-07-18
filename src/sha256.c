#include "sha256.h"

#include <stdint.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr32(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void process_block(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];

    for (unsigned i = 0; i < 16; ++i) {
        unsigned j = i * 4u;
        w[i] = ((uint32_t) block[j] << 24)
             | ((uint32_t) block[j + 1] << 16)
             | ((uint32_t) block[j + 2] << 8)
             |  (uint32_t) block[j + 3];
    }

    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

Sha256Digest sha256_compute(const void *data_ptr, size_t length) {
    const uint8_t *data = (const uint8_t *) data_ptr;
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    size_t full_blocks = length / 64u;
    for (size_t i = 0; i < full_blocks; ++i)
        process_block(state, data + (i * 64u));

    size_t remaining = length - (full_blocks * 64u);
    uint8_t tail[128] = { 0 };
    for (size_t i = 0; i < remaining; ++i)
        tail[i] = data[full_blocks * 64u + i];

    tail[remaining] = 0x80;

    unsigned tail_blocks = (remaining >= 56u) ? 2u : 1u;
    uint64_t bit_length = (uint64_t) length * 8u;
    size_t length_offset = (size_t) tail_blocks * 64u - 8u;
    for (unsigned i = 0; i < 8; ++i)
        tail[length_offset + i] = (uint8_t) (bit_length >> (56u - i * 8u));

    for (unsigned i = 0; i < tail_blocks; ++i)
        process_block(state, tail + i * 64u);

    Sha256Digest digest;
    for (unsigned i = 0; i < 8; ++i) {
        digest.bytes[i * 4u]     = (uint8_t) (state[i] >> 24);
        digest.bytes[i * 4u + 1] = (uint8_t) (state[i] >> 16);
        digest.bytes[i * 4u + 2] = (uint8_t) (state[i] >> 8);
        digest.bytes[i * 4u + 3] = (uint8_t) state[i];
    }

    return digest;
}

int sha256_equal(const Sha256Digest *a, const Sha256Digest *b) {
    uint8_t different = 0;
    for (unsigned i = 0; i < 32; ++i)
        different |= (uint8_t) (a->bytes[i] ^ b->bytes[i]);
    return different == 0;
}
