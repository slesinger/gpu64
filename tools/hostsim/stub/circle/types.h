//
// gpu64 host sim: a stand-in for Circle's <circle/types.h>.
//
// It exists so the portable half of the milestone 6 renderer -- the maths,
// the rasteriser, the colormap builder, the mesh parsers -- can be compiled
// and debugged with a native g++ on a PC, where a wrong pixel costs a second
// instead of an SD card swap and a power cycle. Nothing in this file is
// compiled into the firmware; the firmware gets the real Circle header.
//
// Only the names the portable sources actually use are here. If a source
// needs something else out of Circle, that is the signal it is not portable
// and does not belong on this side of the line.
//
#ifndef _circle_types_h
#define _circle_types_h

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef int boolean;
#define FALSE 0
#define TRUE  1

#endif
