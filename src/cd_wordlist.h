#pragma once

#include <stddef.h>
#include <stdint.h>
#include <psxcd.h>

#define WORDLIST_SECTOR_SIZE 2048u
#define WORDLIST_CHUNK_SECTORS 16u
#define WORDLIST_CHUNK_SIZE (WORDLIST_SECTOR_SIZE * WORDLIST_CHUNK_SECTORS)

typedef struct {
    CdlFILE file;
    uint32_t start_lba;
    uint32_t file_size;
    uint32_t next_sector;
    uint32_t consumed;
    uint32_t chunk_pos;
    uint32_t chunk_size;
    int failed;
    uint8_t buffer[WORDLIST_CHUNK_SIZE] __attribute__((aligned(4)));
} WordlistReader;

int wordlist_open(WordlistReader *reader, const char *path);
int wordlist_rewind(WordlistReader *reader);
int wordlist_next(WordlistReader *reader, char *candidate, size_t capacity, size_t *length);
