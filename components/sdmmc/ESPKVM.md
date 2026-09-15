# ESP-KVM copy of sdmmc

Copied from ESP-IDF v6.1 (`components/sdmmc`, test apps left out). It overrides
the IDF component of the same name. Changes are marked `espkvm:`.

- `sdmmc_cmd.c`: a failed multi-block write returned ESP_OK whenever ACMD22
  (count of written blocks) answered, so part of the data was silently lost.
  The write's own error is kept now.
- `sdmmc_cmd.c`: DMA sector reads and writes are tried up to five times (the bus
  can step down a few clock speeds within one transfer).
- `sdmmc_cmd.c`: each failed attempt calls the weak
  `sdmmc_espkvm_transfer_failed()`, which the application uses to lower the
  bus clock before the retry. `sdmmc_espkvm_transfer_started()` marks each DMA
  read or write, so the application can tell when the card is idle.

Re-check each against upstream when IDF is bumped.
