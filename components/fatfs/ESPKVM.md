# ESP-KVM copy of fatfs

Copied from ESP-IDF v6.1 (`components/fatfs`, test apps and host tests left out).
It overrides the IDF component of the same name. Changes are marked `espkvm:`.

- `src/ffconf.h`: `FF_FS_EXFAT 1`. Cards above 32 GB come exFAT from the factory,
  and without this they have to be reformatted before the recorder can use them.
  IDF has no Kconfig option for it, so the header is the only place to set it.
  FAT12/16/32 are unaffected: FatFs reads the format off the volume at mount, and
  a card already formatted FAT32 keeps working.
- `src/ffconf.h`: `FF_LBA64 1`, which FatFs allows only with exFAT on. This is
  what lets a card partitioned GPT be read at all, and formatting tools pick GPT
  by themselves above 32 GB - so a card could be formatted correctly and still
  not be seen. `FF_MIN_GPT` is left alone; we never format a card.

What this costs, measured on the funcev build (2026-09-23):

- exFAT: 8 080 bytes of flash, 1 997 343 -> 2 005 423. GPT on top: another 4 286,
  2 005 423 -> 2 009 709. Together 12 366 bytes, about 0.3% of an app slot.
- 64 bytes of static internal RAM in total.
- 608 bytes more per FatFs call that takes a path, from the heap and freed again
  (the exFAT directory-entry scratchpad). Our buffers prefer PSRAM.

`CONFIG_FATFS_USE_LABEL=y` in `sdkconfig.defaults` goes with it: with exFAT on,
`ff.c` reads that setting outside a preprocessor test, so the build fails without
it. It also gives us `f_getlabel`, which the console could show one day.

exFAT is patented by Microsoft. They licensed the implementation in the Linux
kernel through OIN; a FatFs build in a product is not covered by that, which is
worth knowing for a project that publishes images.

Re-check against upstream when IDF is bumped: the only edit is the one line in
`ffconf.h`, so a fresh copy plus that line is the whole procedure.
