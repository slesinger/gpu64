#!/usr/bin/env python3
"""
Generate gpu64's HDMI font tables from the Commodore 64 character ROM.

Writes two files, both marked generated:

    Source/Firmware/gpu64_c64font.cpp   the firmware's const font
    tools/prgsim/gpu64font.py           the same bytes for the PC model

Both used to be derived from Source/Firmware/font.h -- RAD's own font.bin.
That is a C64-*style* face, not the machine's: its glyphs are six scanlines
tall where the ROM's are seven, so every character sat a pixel low with a
blank row above it, and 290 of the 512 glyphs differ from the ROM outright
(all 26 letters and all 10 digits among them). The gap a C64 owner sees above
every letter is that one-row shift. This script replaces the lot with the
real 901225-01 dump, which also carries genuine reverse-video glyphs at codes
$80-$FF -- so a cursor over a space is a solid 8x8 block.

Usage:

    tools/gen_c64font.py [path/to/chargen.bin]

The default path is VICE's own copy; GPU64_CHARGEN overrides it. Any
4096-byte C64 chargen dump works -- the script warns, but proceeds, if the
MD5 is not 901225-01's.
"""

import hashlib
import os
import sys

# VICE ships the genuine dump, and a developer box usually has it already.
DEFAULT_CHARGEN = "/usr/share/vice/C64/chargen-901225-01.bin"
CHARGEN_MD5 = "12a4202f5331d45af846af6c58fba946"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP_PATH = os.path.join(ROOT, "Source", "Firmware", "gpu64_c64font.cpp")
PY_PATH = os.path.join(ROOT, "tools", "prgsim", "gpu64font.py")

CHARSET_NAMES = ("uppercase / graphics", "lowercase / uppercase")

# ASCII -> screen code. These are screen codes, not ROM bytes, so the table is
# this script's own data rather than something read out of the dump; it is
# carried over verbatim from the hand-written original.
ASCII_TO_SCREEN = (
    (
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x5C, 0x1D, 0x1E, 0x64,
        0x27, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x5D, 0x1D, 0x40, 0x20,
    ),
    (
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x00, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x1B, 0x5C, 0x1D, 0x1E, 0x64,
        0x27, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x5D, 0x1D, 0x40, 0x20,
    ),
)

CPP_HEADER = """/*
 gpu64: the Commodore 64 character ROM, as an immutable 8x8 font.

 Every text surface gpu64 paints on HDMI renders with the C64's own glyphs --
 the boot screen (the wordmark and the on-screen log, rad_main.cpp and
 gpu64_fb.cpp), the 80x50 text mode (gpu64_text.cpp), and the mirrored C64
 screen (CRAD::showMirror()), so a mirrored BASIC screen is the same pixels
 the C64 is showing beside it.

 Why a private copy rather than font.h's font_bin[]: font_bin is not the C64
 ROM at all -- it is RAD's own lookalike face, six scanlines tall where the
 ROM is seven, which is the extra gap above every character a C64 owner sees
 -- and rad_hijack.cpp *mutates* it in place while the RAD menu is up. The
 menu's oscilloscope trace writes font_bin[64*8 ...], covering screen codes
 $40-$DF, and the damage outlives the menu. Screen code $A0, the reverse
 space a blinking cursor lands on, sits at offset 1280, squarely inside that
 range: drawing the mirror from font_bin left the cursor as a single
 scribbled row instead of a filled cell. This copy is const and is never
 written.

 Layout: [charset][screen code][row]. Charset 0 is the uppercase/graphics
 set, charset 1 the lowercase/uppercase set -- the same two halves the real
 ROM has at $D000 and $D800, and the same pair a C64 program picks between by
 poking $D018. Row 0 is the top scanline, bit 7 the leftmost pixel. Codes
 $80-$FF are the ROM's own reverse-video half, so reverse video is a glyph
 lookup and not a bit inversion -- including the ROM's one asymmetry, where
 reverse @ ($80) differs from ~$00 by a single pixel.

 Generated by tools/gen_c64font.py from %s -- do not edit by hand.
*/
#include "gpu64_c64font.h"

const u8 gpu64C64Font[ 2 ][ 256 ][ 8 ] =
{
"""

CPP_ASCII_COMMENT = """
// ASCII -> screen code. Two rows because the letter half of the ROM moves:
// in charset 0 the alphabet sits at codes 1-26 and there is no lowercase at
// all (both cases fold onto the capitals); in charset 1 lowercase is 1-26 and
// uppercase is 65-90. Everything outside the ROM's repertoire -- the control
// codes, and the four ASCII punctuation marks the C64 never had -- maps to
// the nearest glyph that exists, or to a space, rather than to a random
// graphic:
//
//   '\\\\' -> the closest diagonal graphic (the ROM has no backslash in the
//           lowercase set; the diagonals live only in charset 0)
//   '{' '}' -> '[' ']'          '`' -> apostrophe
//   '^' -> the up arrow          '_' -> the bottom rule
//   '|' -> the vertical rule     '~' -> the mid-height rule
const u8 gpu64C64AsciiToScreen[ 2 ][ 128 ] =
{
"""

PY_HEADER = '''"""
The Commodore 64 character ROM, as the model sees it.

Generated by tools/gen_c64font.py from %s -- the same dump the firmware's
gpu64_c64font.cpp is built from, and deliberately the same source rather than
re-derived, unlike everything else in this directory. gpu64model.py is a
second opinion about *behaviour*; a font is data, and a model carrying an
independently-typed copy of 4096 ROM bytes would only ever produce false
failures. What the model still checks independently is which glyph a given
cell picks and where its pixels land.

Layout: FONT[charset][screen code] is an 8-tuple of row bitmaps, row 0 at the
top, bit 7 leftmost. Codes $80-$FF are the ROM's own reverse-video half.
ASCII_TO_SCREEN[charset][byte] maps a 7-bit ASCII code to the screen code
TEXT_WRITE's ASCII flag would translate it to.
"""

CHARSET_UPPER, CHARSET_LOWER = 0, 1

FONT = (
'''

PY_FOOTER = '''

def screen_code(ch, charset):
    """One ASCII byte to a screen code. Non-ASCII becomes a space, as the
    firmware's gpu64_c64ScreenCode() does."""
    b = ch if isinstance(ch, int) else ord(ch)
    if b >= 0x80:
        return 0x20
    return ASCII_TO_SCREEN[charset & 1][b]
'''


def glyph(rom, charset, code):
    base = charset * 2048 + code * 8
    return rom[base:base + 8]


def emit_cpp(rom, source):
    out = [CPP_HEADER % source]
    for cs in range(2):
        out.append("\t{\t\t\t\t\t// charset %d -- %s\n" % (cs, CHARSET_NAMES[cs]))
        for code in range(256):
            out.append("\t\t{ %s },\t// $%02X\n"
                       % (", ".join("0x%02X" % b for b in glyph(rom, cs, code)), code))
        out.append("\t},\n" if cs == 0 else "\t}\n")
    out.append("};\n")
    out.append(CPP_ASCII_COMMENT)
    for cs in range(2):
        out.append("\t{\t\t\t\t\t// charset %d\n" % cs)
        for i in range(0, 128, 16):
            out.append("\t" + " ".join("0x%02X," % b for b in ASCII_TO_SCREEN[cs][i:i + 16]) + "\n")
        out.append("\t},\n" if cs == 0 else "\t}\n")
    out.append("};\n")
    return "".join(out)


def emit_py(rom, source):
    out = [PY_HEADER % source]
    for cs in range(2):
        out.append("    (   # charset %d -- %s\n" % (cs, CHARSET_NAMES[cs]))
        for code in range(256):
            out.append("        (%s),  # $%02X\n"
                       % (", ".join("0x%02X" % b for b in glyph(rom, cs, code)), code))
        out.append("    ),\n")
    out.append(")\n\nASCII_TO_SCREEN = (\n")
    for cs in range(2):
        out.append("    (\n")
        for i in range(0, 128, 8):
            out.append("        " + " ".join("0x%02X," % b for b in ASCII_TO_SCREEN[cs][i:i + 8]) + "\n")
        out.append("    ),\n")
    out.append(")\n")
    out.append(PY_FOOTER)
    return "".join(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 \
        else os.environ.get("GPU64_CHARGEN", DEFAULT_CHARGEN)
    with open(path, "rb") as f:
        rom = f.read()
    if len(rom) != 4096:
        sys.exit("%s is %d bytes; a C64 chargen dump is 4096" % (path, len(rom)))
    digest = hashlib.md5(rom).hexdigest()
    if digest != CHARGEN_MD5:
        sys.stderr.write("warning: %s md5 %s is not 901225-01's %s\n"
                         % (path, digest, CHARGEN_MD5))
    source = "the C64 character ROM (%s)" % os.path.basename(path)
    for out_path, text in ((CPP_PATH, emit_cpp(rom, source)),
                           (PY_PATH, emit_py(rom, source))):
        with open(out_path, "w") as f:
            f.write(text)
        print("wrote %s" % os.path.relpath(out_path, ROOT))


if __name__ == "__main__":
    main()
