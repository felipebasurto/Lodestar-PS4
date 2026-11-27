/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define FT_LOAD_RENDER 0x4
#define FT_LOAD_NO_BITMAP 0x8

typedef struct {
    long x;
    long y;
} FT_Vector;

typedef struct {
    unsigned int rows;
    unsigned int width;
    int pitch;
    unsigned char *buffer;
} FT_Bitmap;

typedef struct {
    FT_Vector advance;
    int bitmap_left;
    int bitmap_top;
    FT_Bitmap bitmap;
} FT_GlyphSlotRec, *FT_GlyphSlot;

typedef struct {
    int style_id;
    int pixel_size;
    FT_GlyphSlot glyph;
    FT_GlyphSlotRec slot;
    unsigned char glyph_buf[128 * 128];
} FT_FaceRec, *FT_Face;

typedef struct {
    int inited;
} FT_LibraryRec, *FT_Library;

int FT_Init_FreeType(FT_Library *alibrary);
int FT_Done_FreeType(FT_Library library);
int FT_New_Face(FT_Library library, const char *filepathname, long face_index, FT_Face *aface);
unsigned int FT_Get_Char_Index(FT_Face face, unsigned long charcode);
int FT_Set_Pixel_Sizes(FT_Face face, unsigned int pixel_width, unsigned int pixel_height);
int FT_Load_Char(FT_Face face, unsigned long char_code, int load_flags);
