# Character generator fonts

One table per cell height, and a table may hold several fonts - what is looked
up is a bitmap, not "which font is this".

`ibm_vga_8x16.bin` is the IBM VGA text-mode font: 256 glyphs of 16 rows, one
byte per row, most significant bit leftmost - the layout a VGA BIOS keeps in
ROM and the one nearly every legacy PC firmware draws its setup screens with.
It is a dump of that ROM; the bitmaps carry no copyright of their own. FreeBSD's
console font is byte-identical to it.

`pcdos_cp437_8x16.bin` is the PC-DOS code page 437 font, and it is **not** the
same font: 28 glyphs differ, five of them printable ASCII - `` ` `` f v | ~.
This is the font the Linux console carries (`lib/fonts/font_8x16.c`, "generated
by cpi2fnt"), so without it a Linux console reads at about 94% with every f and
v turned into a hole. Taken from viler-int10h's collection of raw VGA text-mode
fonts (`FONTS/SYSTEM/PCDOS2K/CP437.F16`), which is where to look first if some
machine's firmware turns out to draw with a font we do not have:
<https://github.com/viler-int10h/vga-text-mode-fonts>. Not from the kernel
itself, whose copy of it is GPL-2.0.

`uni2_fixed_8x16.psf.gz` is Uni2-Fixed16, the console font a distribution loads
over the one the kernel carries - `console-setup` renders it at boot, and on an
Ubuntu machine it is what is actually on the screen. It is a third drawing
again, not a variant of the two above: one pixel of stroke where they use two,
so a console that has been through `console-setup` reads with neither of them.
Copied unchanged from the `console-setup` package
(`/usr/share/consolefonts/Uni2-Fixed16.psf.gz`), and byte-identical to the
`cached_Uni2-Fixed16.psf.gz` that the machine it was taken from had loaded. That
package's own copyright file settles the licence in one line - "All console
fonts are public domain by nature" - and says separately that the BDF sources
they are built from vary, which is in `copyright.fonts` if a particular one ever
needs tracing. Only hashes of these bitmaps reach the firmware in any case. PSF
carries its own Unicode table, so the file says which character each bitmap is;
`mkfont.py` reads PSF1 and PSF2, gzipped or not.

`uefi_hii_8x19.txt` is the UEFI narrow font: one glyph per line, the Unicode
code point in hex followed by 19 rows. This is what a firmware's own console
draws with, so it is what a UEFI boot menu or setup screen is written in - and
it is a different font, not a taller VGA. It was extracted from EDK2's standard
narrow glyph table (`gUsStdNarrowGlyphData`, Intel, BSD-2-Clause-Patent):

```sh
python3 tools/mkfont.py --from-edk2 LaffStd.c > fonts/uefi_hii_8x19.txt
```

The tables the scanner searches are generated from these:

```sh
python3 tools/mkfont.py fonts/ibm_vga_8x16.bin fonts/pcdos_cp437_8x16.bin \
    fonts/uni2_fixed_8x16.psf.gz --height 16 > screentext_font_h16.h
python3 tools/mkfont.py fonts/uefi_hii_8x19.txt --height 19 > screentext_font_h19.h
```

Only hashes are compiled in, never the bitmaps, so a table costs six bytes a
bitmap whatever the font, and another font of the same height costs only the
glyphs it draws differently. One font is 254 entries; the two VGA-descended ones
together are 282, 224 bytes for the pair; adding Uni2-Fixed16, which shares
nothing with them and carries far more of Unicode, takes it to 782, about 4.7 KB
in all.

Merging has one rule: where two fonts draw the same bitmap for different
characters there is no honest answer, so the bitmap is dropped and a cell that
hits it reads as unreadable. The three here disagree about nothing, so nothing
was dropped. Do not merge indiscriminately for the same reason - all 401
hardware fonts in that collection come to 16112 entries with 2069 of them
ambiguous, which would trade the whole guarantee for coverage nobody asked
for.

A machine that draws with a font in no table will not be read - the scanner
reports too few matches rather than guessing - so the way to support one is to
dump its font, drop it in here and regenerate the table. `setfont -O` writes a
PSF of whatever is on the console; the file `console-setup` cached is under
/etc/console-setup, and the ones it chooses from are in /usr/share/consolefonts.
