#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psxapi.h>
#include <psxcd.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <hwregs_c.h>

#include "cd_wordlist.h"
#include "sha256.h"
#include "splash_assets.h"
#include "wordlist_info.h"

#define SCREEN_XRES 320
#define SCREEN_YRES 240
#define OT_LENGTH 8
#define PACKET_BUFFER_SIZE 32768
#define TEXT_Z 0
#define CANDIDATE_CAPACITY 128
#define HASHES_PER_FRAME 64
#define PROGRESS_CELLS 30

#define TARGET_HASH_HEX "f52fbd32b2b3b86ff88ef6c490628285f482af15ddcb29541f94bcf526a3f6c7"
#define WORDLIST_PATH "\\WORDLIST.TXT"

#define SPLASH_LOGO_X ((SCREEN_XRES - SPLASH_LOGO_WIDTH) / 2)
#define SPLASH_LOGO_Y ((SCREEN_YRES - SPLASH_LOGO_HEIGHT) / 2)
#define SPLASH_TITLE_X ((SCREEN_XRES - SPLASH_TITLE_WIDTH) / 2)
#define SPLASH_TITLE_Y 82

// Texture modulation uses 128 as neutral (full original texture color)
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} UiColor;

static const UiColor COLOR_WHITE   = { 128, 128, 128 };
static const UiColor COLOR_GREY    = {  84,  84,  84 };
static const UiColor COLOR_GREEN   = {   0, 128,   0 };
static const UiColor COLOR_FUCHSIA = { 128,   0, 128 };
static const UiColor COLOR_RED     = { 128,  16,  16 };
static const UiColor COLOR_YELLOW  = { 128, 112,   0 };

// Each render buffer draws into the opposite framebuffer from the one it displays
typedef struct {
    DISPENV disp;
    DRAWENV draw;
    uint32_t ot[OT_LENGTH];
    uint8_t packets[PACKET_BUFFER_SIZE];
} RenderBuffer;

typedef struct {
    RenderBuffer buffers[2];
    uint8_t *next_packet;
    int active;
} RenderContext;

typedef enum {
    SCAN_RUNNING,
    SCAN_PAUSED,
    SCAN_FOUND,
    SCAN_EXHAUSTED,
    SCAN_ABORTED,
    SCAN_IO_ERROR
} ScanStatus;

typedef struct {
    WordlistReader reader;
    Sha256Digest target;
    uint32_t processed;
    uint32_t rate;
    uint32_t rate_base_count;
    int rate_base_vblank;
    char candidate[CANDIDATE_CAPACITY + 1];
    size_t candidate_length;
    ScanStatus status;
} ScanState;

static RenderContext g_render;
static uint8_t g_pad_buffers[2][34];
static uint16_t g_previous_buttons;
static ScanState g_scan;
static uint32_t g_random_state = 0x1f123bb5u;
static TIM_IMAGE g_splash_logo_image;
static TIM_IMAGE g_splash_title_image;
static bool g_splash_assets_ready;

#define SPLASH_LOGO_VRAM_PIXELS \
    ((SPLASH_LOGO_PADDED_WIDTH / 4) * SPLASH_LOGO_HEIGHT)
#define SPLASH_TITLE_VRAM_PIXELS \
    ((SPLASH_TITLE_PADDED_WIDTH / 4) * SPLASH_TITLE_PADDED_HEIGHT)

/*
 * The asset converter enforces these same rules while generating the TIMs.
 * Keeping the fixed target dimensions checked here makes accidental changes
 * fail both at conversion time and compile time.
 */
_Static_assert((SPLASH_LOGO_VRAM_PIXELS % 2) == 0,
               "logo upload must contain an even halfword count");
_Static_assert(((SPLASH_LOGO_VRAM_PIXELS / 2) % 16) == 0,
               "logo upload must use whole DMA blocks");
_Static_assert((SPLASH_TITLE_VRAM_PIXELS % 2) == 0,
               "title upload must contain an even halfword count");
_Static_assert(((SPLASH_TITLE_VRAM_PIXELS / 2) % 16) == 0,
               "title upload must use whole DMA blocks");

static RenderBuffer *active_render_buffer(RenderContext *ctx) {
    return &ctx->buffers[ctx->active];
}

static int packet_space_available(const RenderContext *ctx, size_t bytes) {
    const RenderBuffer *buffer = &ctx->buffers[ctx->active];
    const uint8_t *end = buffer->packets + PACKET_BUFFER_SIZE;
    return ctx->next_packet + bytes <= end;
}

static void *allocate_packet(RenderContext *ctx, int z, size_t size) {
    if (z < 0 || z >= OT_LENGTH || !packet_space_available(ctx, size))
        return NULL;

    RenderBuffer *buffer = active_render_buffer(ctx);
    void *packet = ctx->next_packet;
    ctx->next_packet += size;
    addPrim(&buffer->ot[z], packet);
    return packet;
}

static void reset_packet_buffer(RenderContext *ctx) {
    RenderBuffer *buffer = active_render_buffer(ctx);
    ClearOTagR(buffer->ot, OT_LENGTH);
    ctx->next_packet = buffer->packets;
}

static bool parse_splash_tim(const uint8_t *data, size_t size,
                             int expected_width_words, int expected_height,
                             TIM_IMAGE *image) {
    if (size < 8 || ((const uint32_t *) data)[0] != 0x10u)
        return false;

    memset(image, 0, sizeof(*image));
    GetTimInfo((const uint32_t *) data, image);

    return image->crect && image->caddr && image->prect && image->paddr &&
           image->crect->w == 32 && image->crect->h == 1 &&
           image->prect->w == expected_width_words &&
           image->prect->h == expected_height;
}

static void upload_tim_image(const TIM_IMAGE *image) {
    LoadImage2(image->crect, image->caddr);
    DrawSync(0);
    LoadImage2(image->prect, image->paddr);
    DrawSync(0);
}

static void upload_splash_assets(void) {
    // finish the debug-font transfer before starting either embedded TIM
    DrawSync(0);

    g_splash_assets_ready =
        parse_splash_tim(splash_logo_tim, splash_logo_tim_size,
                         SPLASH_LOGO_PADDED_WIDTH / 4, SPLASH_LOGO_HEIGHT,
                         &g_splash_logo_image) &&
        parse_splash_tim(splash_title_tim, splash_title_tim_size,
                         SPLASH_TITLE_PADDED_WIDTH / 4,
                         SPLASH_TITLE_PADDED_HEIGHT,
                         &g_splash_title_image);

    if (!g_splash_assets_ready)
        return;

    upload_tim_image(&g_splash_logo_image);
    upload_tim_image(&g_splash_title_image);
}

static void init_graphics(RenderContext *ctx) {
    ResetGraph(0);
    FntLoad(960, 0);

    ctx->active = 0;

    SetDefDispEnv(&ctx->buffers[0].disp, 0, 0, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&ctx->buffers[0].draw, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
    SetDefDispEnv(&ctx->buffers[1].disp, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&ctx->buffers[1].draw, 0, 0, SCREEN_XRES, SCREEN_YRES);

    for (int i = 0; i < 2; ++i) {
        setRGB0(&ctx->buffers[i].draw, 0, 0, 0);
        ctx->buffers[i].draw.isbg = 1;
        ctx->buffers[i].draw.dtd = 1;
        ClearOTagR(ctx->buffers[i].ot, OT_LENGTH);
    }

    ctx->next_packet = ctx->buffers[0].packets;

    PutDrawEnv(&ctx->buffers[0].draw);
    PutDispEnv(&ctx->buffers[0].disp);
    upload_splash_assets();
    SetDispMask(0);
}

static void present(RenderContext *ctx) {
    RenderBuffer *buffer = active_render_buffer(ctx);

    DrawSync(0);
    VSync(0);
    PutDispEnv(&buffer->disp);
    DrawOTagEnv(&buffer->ot[OT_LENGTH - 1], &buffer->draw);

    ctx->active ^= 1;
    reset_packet_buffer(ctx);
}

static void draw_text_color(int x, int y, int z, UiColor color, const char *text) {
    size_t max_bytes = strlen(text) * sizeof(SPRT_8) + sizeof(DR_TPAGE);
    if (!packet_space_available(&g_render, max_bytes))
        return;

    RenderBuffer *buffer = active_render_buffer(&g_render);
    uint8_t *start = g_render.next_packet;
    uint8_t *end = (uint8_t *) FntSort(&buffer->ot[z], start, x, y, text);
    uint8_t *glyph_end = end - sizeof(DR_TPAGE);

    for (uint8_t *cursor = start; cursor < glyph_end; cursor += sizeof(SPRT_8)) {
        SPRT_8 *glyph = (SPRT_8 *) cursor;
        setRGB0(glyph, color.r, color.g, color.b);
        setShadeTex(glyph, 0);
        setSemiTrans(glyph, 0);
    }

    g_render.next_packet = end;
}

static void draw_textf_color(int x, int y, int z, UiColor color, const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    draw_text_color(x, y, z, color, line);
}

static void draw_texture_sprite(int x, int y, int width, int height,
                                int u, int v, int texture_x, int texture_y,
                                int clut_x, int clut_y, int z, uint8_t intensity) {
    SPRT *sprite = (SPRT *) allocate_packet(&g_render, z, sizeof(SPRT));
    if (!sprite)
        return;

    setSprt(sprite);
    setXY0(sprite, x, y);
    setWH(sprite, width, height);
    setUV0(sprite, u, v);
    setClut(sprite, clut_x, clut_y);
    setRGB0(sprite, intensity, intensity, intensity);
    setShadeTex(sprite, 0);
    setSemiTrans(sprite, 0);

    DR_TPAGE *tpage = (DR_TPAGE *) allocate_packet(&g_render, z, sizeof(DR_TPAGE));
    if (!tpage)
        return;
    setDrawTPage(tpage, 0, 1, getTPage(0, 0, texture_x, texture_y));
}

static void draw_logo(uint8_t intensity) {
    if (!g_splash_assets_ready)
        return;

    draw_texture_sprite(SPLASH_LOGO_X, SPLASH_LOGO_Y,
                        SPLASH_LOGO_WIDTH, SPLASH_LOGO_HEIGHT,
                        0, 0,
                        g_splash_logo_image.prect->x,
                        g_splash_logo_image.prect->y,
                        g_splash_logo_image.crect->x,
                        g_splash_logo_image.crect->y,
                        2, intensity);
}

static void draw_title_band(int source_y, int height, int offset_x, int offset_y,
                            uint8_t intensity) {
    int screen_x = SPLASH_TITLE_X + offset_x;
    int screen_y = SPLASH_TITLE_Y + source_y + offset_y;

    if (!g_splash_assets_ready)
        return;

    draw_texture_sprite(screen_x, screen_y, 256, height,
                        0, source_y,
                        g_splash_title_image.prect->x,
                        g_splash_title_image.prect->y,
                        g_splash_title_image.crect->x,
                        g_splash_title_image.crect->y,
                        1, intensity);

    draw_texture_sprite(screen_x + 256, screen_y, SPLASH_TITLE_WIDTH - 256, height,
                        0, source_y,
                        g_splash_title_image.prect->x + 64,
                        g_splash_title_image.prect->y,
                        g_splash_title_image.crect->x,
                        g_splash_title_image.crect->y,
                        1, intensity);
}

static void draw_title_clean(uint8_t intensity) {
    draw_title_band(0, SPLASH_TITLE_HEIGHT, 0, 0, intensity);
}

static uint32_t next_random(void) {
    g_random_state = g_random_state * 1664525u + 1013904223u;
    return g_random_state;
}

static int random_range(int minimum, int maximum) {
    uint32_t span = (uint32_t) (maximum - minimum + 1);
    return minimum + (int) (next_random() % span);
}

static void draw_title_glitched(uint8_t intensity, int frame) {
    int source_y = 0;
    while (source_y < SPLASH_TITLE_HEIGHT) {
        int band = random_range(3, 12);
        if (source_y + band > SPLASH_TITLE_HEIGHT)
            band = SPLASH_TITLE_HEIGHT - source_y;

        int offset_x = random_range(-9, 9);
        int offset_y = ((frame & 3) == 0) ? random_range(-2, 2) : 0;
        draw_title_band(source_y, band, offset_x, offset_y, intensity);
        source_y += band;
    }
}

static void init_pad(void) {
    InitPAD(g_pad_buffers[0], sizeof(g_pad_buffers[0]),
            g_pad_buffers[1], sizeof(g_pad_buffers[1]));
    StartPAD();
    ChangeClearPAD(0);
    g_previous_buttons = 0;
}

static uint16_t read_pressed_buttons(void) {
    PADTYPE *pad = (PADTYPE *) g_pad_buffers[0];
    uint16_t held = 0;

    if (pad->stat == 0 && pad->type != PAD_ID_NONE)
        held = (uint16_t) ~pad->btn;

    uint16_t pressed = held & (uint16_t) ~g_previous_buttons;
    g_previous_buttons = held;
    return pressed;
}

static void wait_frames(int frames) {
    for (int i = 0; i < frames; ++i)
        present(&g_render);
}

static int scaled_frames(int ntsc_frames) {
    if (GetVideoMode() == MODE_PAL)
        return (ntsc_frames * 5 + 3) / 6;
    return ntsc_frames;
}

static void splash_scene(void) {
    if (!g_splash_assets_ready)
        return;

    const int fade_in_frames = scaled_frames(180);
    const int glitch_frames = scaled_frames(24);
    const int title_hold_frames = scaled_frames(120);
    const int fade_out_frames = scaled_frames(60);

    for (int frame = 0; frame < fade_in_frames; ++frame) {
        uint8_t intensity = (uint8_t) (((frame + 1) * 128) / fade_in_frames);
        draw_logo(intensity);
        present(&g_render);
    }

    for (int frame = 0; frame < glitch_frames; ++frame) {
        draw_logo(128);
        draw_title_glitched(128, frame);
        present(&g_render);
    }

    for (int frame = 0; frame < title_hold_frames; ++frame) {
        draw_logo(128);
        draw_title_clean(128);
        present(&g_render);
    }

    for (int frame = 0; frame < fade_out_frames; ++frame) {
        uint8_t intensity = (uint8_t) (((fade_out_frames - frame - 1) * 128) / fade_out_frames);
        draw_logo(intensity);
        draw_title_clean(intensity);
        present(&g_render);
    }

    present(&g_render);
    present(&g_render);
}

typedef struct {
    const char *left;
    const char *right;
    UiColor left_color;
    UiColor right_color;
    int right_x;
    int delay;
} BootLine;

static void draw_boot_lines(const BootLine *lines, int count) {
    const int x = 8;
    const int top = 8;
    const int line_height = 10;

    for (int i = 0; i < count; ++i) {
        int y = top + i * line_height;
        if (lines[i].left && lines[i].left[0])
            draw_text_color(x, y, TEXT_Z, lines[i].left_color, lines[i].left);
        if (lines[i].right && lines[i].right[0])
            draw_text_color(lines[i].right_x, y, TEXT_Z, lines[i].right_color, lines[i].right);
    }
}

static void boot_scene(void) {
    const int line_delay = scaled_frames(10);
    const int blank_delay = scaled_frames(5);
    const int hold_frames = scaled_frames(60);

    const BootLine lines[] = {
        { "PSX-HASHCAT v 1.1.4", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "PORT OF SOLST GBA-HASHCAT", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "===============================", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "", NULL, COLOR_WHITE, COLOR_WHITE, 0, blank_delay },
        { "CPU: 33.8688 MHZ MIPS R3000A", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "CORES: 1", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "RAM: 2 MB", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "STORAGE: CD-ROM", NULL, COLOR_WHITE, COLOR_WHITE, 0, line_delay },
        { "WORDLIST: WORDLIST.TXT", "[LOADED]", COLOR_WHITE, COLOR_GREEN, 232, line_delay },
        { "", NULL, COLOR_WHITE, COLOR_WHITE, 0, blank_delay },
        { "TARGET HASH:", NULL, COLOR_FUCHSIA, COLOR_FUCHSIA, 0, line_delay },
        { "F52FBD32B2B3B86FF88EF6C490628285", NULL, COLOR_FUCHSIA, COLOR_FUCHSIA, 0, line_delay },
        { "F482AF15DDCB29541F94BCF526A3F6C7", NULL, COLOR_FUCHSIA, COLOR_FUCHSIA, 0, line_delay }
    };
    const int line_count = (int) (sizeof(lines) / sizeof(lines[0]));

    for (int visible = 1; visible <= line_count; ++visible) {
        int delay = lines[visible - 1].delay;
        for (int frame = 0; frame < delay; ++frame) {
            draw_boot_lines(lines, visible);
            present(&g_render);
        }
    }

    for (int frame = 0; frame < hold_frames; ++frame) {
        draw_boot_lines(lines, line_count);
        present(&g_render);
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_digest(const char *hex, Sha256Digest *digest) {
    for (unsigned i = 0; i < 32; ++i) {
        int high = hex_nibble(hex[i * 2u]);
        int low = hex_nibble(hex[i * 2u + 1u]);
        if (high < 0 || low < 0)
            return 0;
        digest->bytes[i] = (uint8_t) ((high << 4) | low);
    }
    return hex[64] == '\0';
}

static void sanitize_candidate(const char *source, size_t length, char *output, size_t output_size) {
    if (output_size == 0)
        return;

    size_t limit = output_size - 1;
    if (length > limit)
        length = limit;

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char) source[i];
        output[i] = (c >= 32 && c <= 126) ? (char) c : '.';
    }
    output[length] = '\0';
}

static void make_progress_bar(char *bar, size_t capacity, uint32_t processed) {
    if (capacity < PROGRESS_CELLS + 3)
        return;

    uint32_t filled = (uint32_t) (((uint64_t) processed * PROGRESS_CELLS) / WORDLIST_ENTRY_COUNT);
    if (filled > PROGRESS_CELLS)
        filled = PROGRESS_CELLS;

    bar[0] = '[';
    for (unsigned i = 0; i < PROGRESS_CELLS; ++i)
        bar[i + 1] = (i < filled) ? '#' : '.';
    bar[PROGRESS_CELLS + 1] = ']';
    bar[PROGRESS_CELLS + 2] = '\0';
}

static const char *status_text(ScanStatus status) {
    switch (status) {
        case SCAN_RUNNING:   return "RUNNING";
        case SCAN_PAUSED:    return "PAUSED";
        case SCAN_FOUND:     return "MATCH FOUND";
        case SCAN_EXHAUSTED: return "NO MATCH";
        case SCAN_ABORTED:   return "ABORTED";
        case SCAN_IO_ERROR:  return "CD READ ERROR";
        default:             return "UNKNOWN";
    }
}

static UiColor status_color(ScanStatus status) {
    switch (status) {
        case SCAN_RUNNING:
        case SCAN_FOUND:
            return COLOR_GREEN;
        case SCAN_PAUSED:
            return COLOR_YELLOW;
        case SCAN_EXHAUSTED:
        case SCAN_ABORTED:
        case SCAN_IO_ERROR:
            return COLOR_RED;
        default:
            return COLOR_WHITE;
    }
}

static void draw_scan_ui(const ScanState *scan) {
    char bar[PROGRESS_CELLS + 3];
    char display_candidate[33];
    make_progress_bar(bar, sizeof(bar), scan->processed);
    sanitize_candidate(scan->candidate, scan->candidate_length,
                       display_candidate, sizeof(display_candidate));

    uint32_t percent_tenths = (uint32_t) (((uint64_t) scan->processed * 1000u) / WORDLIST_ENTRY_COUNT);
    if (percent_tenths > 1000u)
        percent_tenths = 1000u;

    draw_text_color(8, 8, TEXT_Z, COLOR_WHITE, "PS1-HASHCAT :: SHA-256");
    draw_text_color(8, 18, TEXT_Z, COLOR_WHITE, "======================================");
    draw_text_color(8, 30, TEXT_Z, COLOR_FUCHSIA, "TGT F52FBD32B2B3B86FF88EF6C490628285");
    draw_text_color(8, 40, TEXT_Z, COLOR_FUCHSIA, "    F482AF15DDCB29541F94BCF526A3F6C7");

    draw_text_color(8, 62, TEXT_Z, COLOR_GREEN, bar);
    draw_textf_color(8, 74, TEXT_Z, COLOR_WHITE, "%3lu.%1lu%%  %lu / %lu",
                     (unsigned long) (percent_tenths / 10u),
                     (unsigned long) (percent_tenths % 10u),
                     (unsigned long) scan->processed,
                     (unsigned long) WORDLIST_ENTRY_COUNT);
    draw_textf_color(8, 86, TEXT_Z, COLOR_GREEN, "RATE: %lu H/S", (unsigned long) scan->rate);
    draw_text_color(8, 98, TEXT_Z, COLOR_WHITE, "STATUS:");
    draw_text_color(72, 98, TEXT_Z, status_color(scan->status), status_text(scan->status));

    draw_text_color(8, 122, TEXT_Z, COLOR_WHITE, "TRY:");
    draw_text_color(48, 122, TEXT_Z, COLOR_WHITE, display_candidate);

    draw_text_color(8, 164, TEXT_Z, COLOR_GREY, "START PAUSE/RESUME   CIRCLE ABORT");
    draw_text_color(8, 176, TEXT_Z, COLOR_GREY, "SELECT RESTART AFTER COMPLETION");

    if (scan->status == SCAN_FOUND) {
        draw_text_color(8, 144, TEXT_Z, COLOR_GREEN, "*** MATCH FOUND ***");
        draw_text_color(8, 188, TEXT_Z, COLOR_GREEN, "PLAINTEXT:");
        draw_text_color(96, 188, TEXT_Z, COLOR_GREEN, display_candidate);
    } else if (scan->status == SCAN_EXHAUSTED) {
        draw_text_color(8, 144, TEXT_Z, COLOR_RED, "*** CRACKING FAILED ***");
        draw_text_color(8, 188, TEXT_Z, COLOR_RED, "NO MATCH IN WORDLIST.");
    } else if (scan->status == SCAN_IO_ERROR) {
        draw_text_color(8, 144, TEXT_Z, COLOR_RED, "*** CD READ ERROR ***");
        draw_text_color(8, 188, TEXT_Z, COLOR_RED, "COULD NOT CONTINUE READING DISC.");
    } else if (scan->status == SCAN_ABORTED) {
        draw_text_color(8, 144, TEXT_Z, COLOR_RED, "*** SCAN ABORTED ***");
    } else if (scan->status == SCAN_PAUSED) {
        draw_text_color(8, 144, TEXT_Z, COLOR_YELLOW, "*** PAUSED ***");
    }
}

static int reset_scan(ScanState *scan) {
    memset(scan, 0, sizeof(*scan));

    if (!parse_digest(TARGET_HASH_HEX, &scan->target)) {
        scan->status = SCAN_IO_ERROR;
        return 0;
    }

    if (!wordlist_open(&scan->reader, WORDLIST_PATH)) {
        scan->status = SCAN_IO_ERROR;
        return 0;
    }

    scan->status = SCAN_RUNNING;
    scan->rate_base_vblank = VSync(-1);
    return 1;
}

static void update_rate(ScanState *scan) {
    int now = VSync(-1);
    int elapsed = now - scan->rate_base_vblank;
    int refresh_rate = (GetVideoMode() == MODE_PAL) ? 50 : 60;

    if (elapsed >= refresh_rate) {
        uint32_t delta = scan->processed - scan->rate_base_count;
        scan->rate = (uint32_t) (((uint64_t) delta * (uint32_t) refresh_rate) / (uint32_t) elapsed);
        scan->rate_base_count = scan->processed;
        scan->rate_base_vblank = now;
    }
}

static void process_scan_batch(ScanState *scan) {
    if (scan->status != SCAN_RUNNING)
        return;

    for (int i = 0; i < HASHES_PER_FRAME; ++i) {
        size_t length = 0;
        if (!wordlist_next(&scan->reader, scan->candidate, CANDIDATE_CAPACITY, &length)) {
            scan->candidate_length = 0;
            scan->status = scan->reader.failed ? SCAN_IO_ERROR : SCAN_EXHAUSTED;
            break;
        }

        scan->candidate_length = length;
        scan->candidate[length] = '\0';

        Sha256Digest digest = sha256_compute(scan->candidate, length);
        ++scan->processed;

        if (sha256_equal(&digest, &scan->target)) {
            scan->status = SCAN_FOUND;
            break;
        }
    }

    update_rate(scan);
}

static void run_cracker(void) {
    reset_scan(&g_scan);

    for (;;) {
        uint16_t pressed = read_pressed_buttons();

        if (pressed & PAD_START) {
            if (g_scan.status == SCAN_RUNNING)
                g_scan.status = SCAN_PAUSED;
            else if (g_scan.status == SCAN_PAUSED) {
                g_scan.status = SCAN_RUNNING;
                g_scan.rate_base_count = g_scan.processed;
                g_scan.rate_base_vblank = VSync(-1);
            }
        }

        if ((pressed & PAD_CIRCLE) &&
            (g_scan.status == SCAN_RUNNING || g_scan.status == SCAN_PAUSED))
            g_scan.status = SCAN_ABORTED;

        if ((pressed & PAD_SELECT) &&
            (g_scan.status == SCAN_FOUND || g_scan.status == SCAN_EXHAUSTED ||
             g_scan.status == SCAN_ABORTED || g_scan.status == SCAN_IO_ERROR))
            reset_scan(&g_scan);

        process_scan_batch(&g_scan);
        draw_scan_ui(&g_scan);
        present(&g_render);
    }
}

int main(void) {
    init_graphics(&g_render);

    // clear both framebuffer pages before enabling video output
    present(&g_render);
    present(&g_render);
    DrawSync(0);
    VSync(0);
    SetDispMask(1);

    init_pad();
    CdInit();

    splash_scene();
    boot_scene();
    wait_frames(scaled_frames(10));
    run_cracker();
    return 0;
}
