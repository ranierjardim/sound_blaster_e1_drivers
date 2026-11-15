/*
 * mkicon.c - build-time tool: renders the E1 Studio emblem (src/panel/gfx.c, our own vector
 * artwork) at the standard icon sizes and writes a multi-resolution .ico (32-bit BMP entries).
 * Usage: mkicon <out.ico>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/panel/gfx.h"

static const int k_sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
#define NSIZES ((int)(sizeof k_sizes / sizeof k_sizes[0]))

#pragma pack(push, 1)
typedef struct { WORD reserved, type, count; } IconDir;
typedef struct { BYTE w, h, colors, reserved; WORD planes, bpp; DWORD bytes, offset; } IconDirEntry;
#pragma pack(pop)

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { fwprintf(stderr, L"usage: mkicon <out.ico>\n"); return 2; }
    gfx_startup();
    unsigned char *img[NSIZES];
    DWORD bytes[NSIZES];
    for (int i = 0; i < NSIZES; ++i) {
        int s = k_sizes[i];
        unsigned char *bgra = (unsigned char *)malloc((size_t)s * s * 4);
        if (!bgra || !gfx_render_emblem_bgra(s, bgra)) { fwprintf(stderr, L"render %d failed\n", s); return 1; }
        DWORD mask_stride = (DWORD)(((s + 31) / 32) * 4);
        bytes[i] = (DWORD)(sizeof(BITMAPINFOHEADER) + (size_t)s * s * 4 + mask_stride * (DWORD)s);
        img[i] = (unsigned char *)calloc(1, bytes[i]);
        if (!img[i]) return 1;
        BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)img[i];
        bh->biSize = sizeof *bh;
        bh->biWidth = s;
        bh->biHeight = s * 2;
        bh->biPlanes = 1;
        bh->biBitCount = 32;
        bh->biCompression = BI_RGB;
        bh->biSizeImage = (DWORD)(s * s * 4);
        unsigned char *xorp = img[i] + sizeof *bh;
        unsigned char *andp = xorp + (size_t)s * s * 4;
        for (int y = 0; y < s; ++y) {
            memcpy(xorp + (size_t)(s - 1 - y) * s * 4, bgra + (size_t)y * s * 4, (size_t)s * 4); /* bottom-up */
            for (int x = 0; x < s; ++x)
                if (bgra[((size_t)y * s + x) * 4 + 3] == 0) andp[(size_t)(s - 1 - y) * mask_stride + x / 8] |= (unsigned char)(0x80 >> (x % 8));
        }
        free(bgra);
    }
    FILE *f = _wfopen(argv[1], L"wb");
    if (!f) { fwprintf(stderr, L"cannot write %s\n", argv[1]); return 1; }
    IconDir d = {0, 1, (WORD)NSIZES};
    fwrite(&d, sizeof d, 1, f);
    DWORD off = (DWORD)(sizeof d + sizeof(IconDirEntry) * NSIZES);
    for (int i = 0; i < NSIZES; ++i) {
        IconDirEntry e;
        e.w = (BYTE)(k_sizes[i] >= 256 ? 0 : k_sizes[i]);
        e.h = e.w;
        e.colors = 0;
        e.reserved = 0;
        e.planes = 1;
        e.bpp = 32;
        e.bytes = bytes[i];
        e.offset = off;
        off += bytes[i];
        fwrite(&e, sizeof e, 1, f);
    }
    for (int i = 0; i < NSIZES; ++i) { fwrite(img[i], bytes[i], 1, f); free(img[i]); }
    fclose(f);
    gfx_shutdown();
    wprintf(L"mkicon: wrote %s (%d sizes)\n", argv[1], NSIZES);
    return 0;
}
