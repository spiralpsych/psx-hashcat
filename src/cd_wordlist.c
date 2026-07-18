#include "cd_wordlist.h"

#include <stdint.h>
#include <string.h>

static int refill(WordlistReader *reader) {
    if (reader->consumed >= reader->file_size)
        return 0;

    uint32_t remaining = reader->file_size - reader->consumed;
    uint32_t sectors = (remaining + WORDLIST_SECTOR_SIZE - 1u) / WORDLIST_SECTOR_SIZE;
    if (sectors > WORDLIST_CHUNK_SECTORS)
        sectors = WORDLIST_CHUNK_SECTORS;

    CdlLOC location;
    CdIntToPos((int) (reader->start_lba + reader->next_sector), &location);

    if (!CdControl(CdlSetloc, (const uint8_t *) &location, 0)) {
        reader->failed = 1;
        return 0;
    }
    if (!CdRead((int) sectors, (uint32_t *) reader->buffer, CdlModeSpeed)) {
        reader->failed = 1;
        return 0;
    }
    if (CdReadSync(0, 0) < 0) {
        reader->failed = 1;
        return 0;
    }

    uint32_t bytes_read = sectors * WORDLIST_SECTOR_SIZE;
    reader->chunk_size = (remaining < bytes_read) ? remaining : bytes_read;
    reader->chunk_pos = 0;
    reader->next_sector += sectors;
    return 1;
}

static int read_byte(WordlistReader *reader, uint8_t *value) {
    if (reader->chunk_pos >= reader->chunk_size) {
        if (!refill(reader))
            return 0;
    }

    *value = reader->buffer[reader->chunk_pos++];
    ++reader->consumed;
    return 1;
}

int wordlist_open(WordlistReader *reader, const char *path) {
    memset(reader, 0, sizeof(*reader));

    if (!CdSearchFile(&reader->file, path)) {
        reader->failed = 1;
        return 0;
    }

    reader->start_lba = (uint32_t) CdPosToInt(&reader->file.pos);
    reader->file_size = (uint32_t) reader->file.size;
    return 1;
}

int wordlist_rewind(WordlistReader *reader) {
    reader->next_sector = 0;
    reader->consumed = 0;
    reader->chunk_pos = 0;
    reader->chunk_size = 0;
    reader->failed = 0;
    return 1;
}

int wordlist_next(WordlistReader *reader, char *candidate, size_t capacity, size_t *length) {
    size_t used = 0;
    int got_data = 0;
    uint8_t value;

    while (read_byte(reader, &value)) {
        got_data = 1;

        if (value == '\n')
            break;
        if (value == '\r')
            continue;

        if (used < capacity)
            candidate[used++] = (char) value;
    }

    if (!got_data)
        return 0;

    *length = used;
    return 1;
}
