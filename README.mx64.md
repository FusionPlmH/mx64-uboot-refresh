# MX64 U-Boot (ng)

This tree starts from the stock Meraki MX64 U-Boot GPL source and adds the
board support needed to boot OpenWrt, plus Ethernet/TFTP recovery and serial
diagnostics.

## Source provenance

- Stock source: `https://dl.meraki.net/U-boot-MX64-20190430.tar.bz2`
- SHA256: `19b5c65e7876c828dca821c8cd6f377ba04a2e68870605a8c57559ce61e72f13`
- Base: U-Boot 2012.10 + Broadcom NSP SDK
- Board support imported from:
  `https://github.com/clayface/U-boot-MX64-20190430_MX64` at `cd5b491`

The initial commit is the unmodified stock tree. The clayface board-support
changes are kept as a separate import commit so the delta remains reviewable.

## Branches

- `main` - stock + clayface board support + networking/TFTP/diagnostics
- `stock` - unmodified 20190430 source import
- `clayface-import` - stock plus the imported clayface board support

## Build

Use a Linux/WSL shell. Two toolchains are known to work:

- Bootlin `armv7-eabihf--glibc--stable` (GCC 5.4), prefix
  `arm-buildroot-linux-gnueabihf-`. It is preconfigured for `vfpv3-d16`, so no
  extra flags are needed.
- A distribution cross compiler, e.g. Debian/Ubuntu
  `gcc-arm-linux-gnueabihf` (GCC 13). Those default to `-mfloat-abi=hard`
  without a default FPU, so `-march=armv7-a` cannot be probed and the tree
  silently falls back to `-march=armv5`, which modern GCC rejects. Pass an FPU
  explicitly.

`CROSS_COMPILE` must be exported for the configuration step as well: the
`cc-option` results are cached there, and a cache generated with the host
compiler makes the build fall back to unusable flags.

```sh
export CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make kingpin_nand_config
make -j8
```

```sh
export CROSS_COMPILE=arm-linux-gnueabihf-
make CROSS_COMPILE=$CROSS_COMPILE CC="${CROSS_COMPILE}gcc -mfpu=vfpv3-d16" kingpin_nand_config
make CROSS_COMPILE=$CROSS_COMPILE CC="${CROSS_COMPILE}gcc -mfpu=vfpv3-d16" -j8
```

`config.mk` assigns `CC = $(CROSS_COMPILE)gcc`, so `CC` has to be overridden on
the make command line (exporting it does nothing).

The output is `u-boot.bin`: 591,984 bytes with GCC 13 and 598,268 bytes with
GCC 5.4 (v5). The upstream MX6x device tree declares the `u-boot` partition as
`0x80000` (512 KiB) while other units use `0x100000` (1 MiB), so read the real
size (`nand info`, or `cat /proc/mtd` on a running system) before writing: never
write an image that is larger than the partition.

The stock tree forces `-msoft-float`; this repo lets the toolchain float ABI
match its libgcc. The tested Bootlin `eabihf` toolchain therefore produces a
hard-float ABI binary, but U-Boot itself uses no floating point. Use a
soft-float GCC 5 toolchain if strict ABI compatibility with the stock image is
required.

`kingpin_nand_config` is a normal target in this tree; the accidental empty
marker file from the stock tarball has been removed.

## Hardware defaults

- SoC: BCM58625 (Northstar Plus), Cortex-A9, 2 GB RAM
- NAND: 1 GB, 4 KiB pages, 256 KiB erase blocks
- U-Boot env: NAND offset `0x440000`, size `0x40000`
- Shmoo values: NAND offset `0x480000`
- Stock U-Boot Ethernet mapping: GMAC1 (`CONFIG_GMAC_NUM 1`)
- `gmac` environment variable can override the mapping (`0`, `1`, or `2`)
- Default environment MAC is `00:18:0a:00:01:02`; set the label MAC before
  using TFTP on a shared network:
  `setenv ethaddr 00:18:0a:00:01:02`

The official MX64 firmware source confirms the NAND layout and the reset/LED
GPIOs. Its `board_kingpin.c` maps `senao_nvram` at `0x400000` and the UBI
partition at `0x800000`.

## Boot flow

`CONFIG_BOOTCOMMAND` runs:

```text
run soc_dtscheck; mx64init; run ubi_checkboot; run usb_checkboot; run tftp_checkboot; run meraki_checkboot
```

`mx64init` enables the board peripherals and checks the reset button. If reset
is pressed, USB boot is attempted immediately. Without the button the boot
continues with the normal candidates, and each one only runs when the previous
one failed:

1. `ubi_boot` - `ubi read` the OpenWrt kernel and `bootm` it.
2. `usb_checkboot` - `usbload`, three attempts over both buses and both
   initramfs names (`enable_usb_boot=1`).
3. `tftp_checkboot` - `tftp_boot`, pulling the image over the network
   (`enable_tftp_boot=1`). It unconditionally sets `ipaddr=192.168.1.10`,
   `serverip=192.168.1.2` and `netmask=255.255.255.0` (the same values that are
   compiled in as `CONFIG_IPADDR`/`CONFIG_SERVERIP`/`CONFIG_NETMASK`), so a
   wiped, stale or `saveenv`'d environment cannot point the recovery path at a
   different subnet. It also sets `netretry=no` so a missing server fails after
   one try.
   Candidates, in order: an explicit `tftp_file` override, the OpenWrt FIT
   initramfs (`initramfs_filename[_a0]`), then the clayface `SPIM` transition
   firmware (`transition_filename[_a0]`). `bootimg` identifies each one from its
   header, so the first file that exists and boots wins.
4. `meraki_checkboot` - the stock `bootkernel1/2` partitions
   (`enable_meraki_boot=0`).

That chain is why an empty or corrupt NAND boots itself from a USB stick or over
TFTP without anybody holding the reset button. The flashing scripts in
mx64-openwrt-flash talk TFTP by default, which does not change anything here.

### Rewriting the boot loader over TFTP

```text
flash_uboot <tftp-file> <partition-size-hex> confirm
```

`tftpboot`s the file, refuses it unless the first word is an ARM branch
instruction and the length fits the given partition, then `nand erase`s the
partition, writes the image, reads it back and compares CRC32. Success prints
`OK: ... written and verified`; a mismatch prints `VERIFY FAILED` and leaves the
image in RAM so the write can be retried.

The partition size is mandatory because the erase covers the whole partition and
`shmoo` starts right behind a `0x80000` mtd0. Power loss during the write bricks
the unit, so keep a serial console attached.

### Image formats

`bootimg <addr> [partition]` looks at the first word of the loaded image and
picks the boot method from it, so the USB and TFTP paths do not have to know
which kind of file they fetched:

- first bytes `d0 0d fe ed` (FIT) or `27 05 19 56` (legacy uImage) are booted
  with `bootm`. FIT images get `${config_dts}` appended as `#config@N`.
- first bytes `53 50 49 4d` (ASCII `SPIM`, the Meraki transition /
  initramfs-kernel image) are copied to the `load_addr` recorded in its own
  header (normally `0x60008000`) and entered with `bootbk <addr> bootkernel1`,
  because that loader jumps to an absolute entry point.

`bootbk` verifies the header CRC32, rejects an empty or oversized payload and
restores the CRC field afterwards, so a failed boot can be retried on the same
buffer.

OpenWrt's `openwrt-bcm53xx-generic-meraki_mx64-initramfs.bin` is a FIT image
and goes through `bootm`. The clayface
`openwrt-bcm5862x-generic-meraki_mx64-initramfs-kernel.bin` is the `SPIM`
transition firmware and must go through `bootbk`; `bootimg` selects it
automatically, so both can be tried from one USB stick or one TFTP directory.

## A trap to avoid: U-Boot's shell cannot test for an empty variable

Two releases in a row were shipped with a `config_dts` that was silently never
set, because both spellings below look correct and neither works:

```text
# v2 -- hush does not propagate a command's return value into $?, so this is
#       never true and every unit fell through to config@2 (A0)
getsocrev; if test $? = 1; then setenv config_dts config@4; else ...; fi

# v3 -- with config_dts empty this collapses to "test -z" (two arguments), and
#       common/cmd_test.c starts with "if (argc < 3) return 1;", i.e. false,
#       so the then-branch never ran
if test -z "${config_dts}"; then getsocrev set; fi
```

`get_socrev()` itself was never wrong -- the disassembly is a straightforward
`ubfx r0, r0, #16, #4 / cmp #3 / movhi #1` over `ICFG_CHIP_ID_REG`.

From v4 on, `soc_dtscheck` is a bare, unconditional `getsocrev set` and the
"do not override an explicit value" check lives in C:

```c
if (argc >= 2 && !strcmp(argv[1], "set")) {
        const char *cur = getenv("config_dts");
        if (cur && *cur)
                return 0;
        setenv("config_dts", b0 ? "config@4" : "config@2");
}
```

Rule of thumb: keep anything conditional in C, not in the environment scripts.

## TFTP recovery

Put the initramfs in the TFTP server root and connect the PC to a LAN port.
For a B0+ unit:

```text
setenv ethaddr 00:18:0a:00:01:02
setenv gmac 1
setenv ipaddr 192.168.1.10
setenv serverip 192.168.1.2
setenv netmask 255.255.255.0
ping 192.168.1.2
tftp_boot
```

`tftp_boot` runs `soc_dtscheck`, forces the recovery IPs, then tries three
candidates in order and boots the first one that loads:

1. `tftp_file`, if the environment sets it (manual override),
2. `openwrt-bcm53xx-generic-meraki_mx64[-a0]-initramfs.bin`,
3. `openwrt-bcm5862x-generic-meraki_mx64[a0]-initramfs-kernel.bin` (the `SPIM`
   transition firmware).

Each one goes through `bootimg`, which uses `bootm` for FIT images and `bootbk`
for the `SPIM` transition firmware. Both `enable_usb_boot` and
`enable_tftp_boot` default to 1, so an empty NAND tries the USB stick first and
the network right after it.

Manual B0+ load:

```text
tftpboot 0x90000000 openwrt-bcm53xx-generic-meraki_mx64-initramfs.bin
bootm 0x90000000#config@4
```

A0 uses the `_a0` filename and `config@2`.

Transition firmware (clayface `openwrt-bcm5862x-...-initramfs-kernel.bin`):

```text
tftpboot 0x90000000 openwrt-bcm5862x-generic-meraki_mx64-initramfs-kernel.bin
bootimg 0x90000000 bootkernel1
```

If `ping` fails, check `mii info`, the LAN port, the PC firewall, and the TFTP
server. If GMAC1 does not link, try `setenv gmac 2; saveenv; reset`.

Serial `loady` remains available as a last-resort transfer path.

## USB boot diagnostics

The USB path tries three times, both USB buses (`usb 0:1`, `usb 1:1`), and both
initramfs filenames. Every successfully loaded file is handed to `bootimg`, so a
FIT initramfs and the `SPIM` transition firmware can sit on the same stick and
the first one that actually boots wins.

LED codes (amber power LED stays on, white status LED blinks):

- 3 white blinks at start: a USB boot attempt begins
- 1 pulse, then retry: no USB storage device was detected (`usb reset` cannot
  report this itself, so the code probes the storage layer)
- 2 pulses, then retry: a stick was present but no configured file could be
  loaded from `usb 0:1` or `usb 1:1`
- 3 pulses, then retry: a file was loaded but it did not boot (`bootm` or
  `bootbk` returned)

Serial output prints the exact `fatload` command, the `bootm`/`bootbk` decision
and `usb info` after every reset. A file that loads but fails to boot does not
abort the attempt: the remaining files and buses are still tried.

## Identification

```text
version
mx64info
```

`mx64info` prints the build tag, SoC revision, reset state, USB settings,
load address, DTS config, IP/MAC and GMAC selection.

## Boot commands

| Command | Purpose |
|---|---|
| `mx64init` | board init + reset button check (runs from `CONFIG_BOOTCOMMAND`) |
| `usbload` | try the USB stick: 3 attempts, both buses, both initramfs names |
| `tftp_boot` | pull an image over TFTP and hand it to `bootimg`: `tftp_file` override, then the FIT initramfs, then the `SPIM` transition firmware |
| `bootimg <addr> [part]` | FIT/uImage -> `bootm`, Meraki `SPIM` -> relocate + `bootbk` |
| `bootbk <addr> <part>` | start a Meraki bootkernel (header CRC32 checked) |
| `flash_uboot <file> <size-hex> confirm` | TFTP + erase + write + read back verify of the boot loader |
| `getsocrev` | exit code 1 on B0+, 0 on A0 (used by `soc_dtscheck`) |
| `mx64info` | dump build/board state |

## Flashing warning

Back up the current `u-boot` and environment partitions and keep a serial
console attached before writing a new bootloader. This tree has been built and
inspected, but the TFTP/USB paths still need validation on hardware.

A ready-to-use flashing kit (guide, scripts, `uboot_mx64`/`uboot_mx64_small`,
transition firmware, initramfs and sysupgrade images) lives in
<https://github.com/FusionPlmH/mx64-openwrt-flash>.
