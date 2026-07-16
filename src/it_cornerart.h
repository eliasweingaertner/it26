/*
 * it_cornerart.h
 * --------------
 * The port's corner-art badge (76x72, 8-entry palette).
 * Generated into it_cornerart.c by tools/gen_cornerart.py from
 * art/corner.bmp.
 */

#ifndef IT_CORNERART_DATA_H
#define IT_CORNERART_DATA_H

#include <stdint.h>

#define IT_CORNERART_W 76
#define IT_CORNERART_H 72

/* 0xRRGGBB per palette index */
extern const uint32_t IT_CornerArtPal[8];
extern const uint8_t IT_CornerArt[IT_CORNERART_H][IT_CORNERART_W];

#endif /* IT_CORNERART_DATA_H */
