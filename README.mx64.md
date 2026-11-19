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

## Build

Use a Linux/WSL shell. The tested toolchain is Bootlin
`armv7-eabihf--glibc--stable` (GCC 5.4), prefix
`arm-buildroot-linux-gnueabihf-`.

```sh
make kingpin_nand_config
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- -j8
```

The output is `u-boot.bin`. A clean build is about 596 KB and fits the MX64
`U-boot` NAND partition.

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
  `setenv ethaddr 0c:8d:db:90:0a:a0`

The official MX64 firmware source confirms the NAND layout and the reset/LED
GPIOs. Its `board_kingpin.c` maps `senao_nvram` at `0x400000` and the UBI
partition at `0x800000`.

## Boot flow

`CONFIG_BOOTCOMMAND` runs:

```text
run soc_dtscheck; mx64init; run ubi_checkboot; run usb_checkboot; run meraki_checkboot
```

`mx64init` enables the board peripherals and checks the reset button. If reset
is pressed, USB boot is attempted; otherwise the normal UBI kernel boot runs.
USB is also retried after a UBI boot failure when `enable_usb_boot=1`.

## TFTP recovery

Put the initramfs in the TFTP server root and connect the PC to a LAN port.
For a B0+ unit:

```text
setenv ethaddr 0c:8d:db:90:0a:a0
setenv gmac 1
setenv ipaddr 192.168.1.10
setenv serverip 192.168.1.2
setenv netmask 255.255.255.0
ping 192.168.1.2
tftp_boot
```

`tftp_boot` runs `soc_dtscheck`, chooses the A0 or B0+ initramfs filename, loads
it to `${loadaddr}`, and boots `${loadaddr}#${config_dts}`.

Manual B0+ load:

```text
tftpboot 0x90000000 openwrt-bcm53xx-generic-meraki_mx64-initramfs.bin
bootm 0x90000000#config@4
```

A0 uses the `_a0` filename and `config@2`.

If `ping` fails, check `mii info`, the LAN port, the PC firewall, and the TFTP
server. If GMAC1 does not link, try `setenv gmac 2; saveenv; reset`.

Serial `loady` remains available as a last-resort transfer path.

## USB boot diagnostics

The USB path tries three times, both USB buses (`usb 0:1`, `usb 1:1`), and both
initramfs filenames.

LED codes:

- 3 white blinks: USB boot attempt starts
- 1 pulse: `usb reset` failed
- 2 pulses: USB reset worked but no FAT load succeeded
- 3 pulses: image loaded but `bootm` failed

Serial output prints each stage and `usb info` after reset.

## Identification

```text
version
mx64info
```

`mx64info` prints the build tag, SoC revision, reset state, USB settings,
load address, DTS config, IP/MAC and GMAC selection.

## Flashing warning

Back up the current `u-boot` and environment partitions and keep a serial
console attached before writing a new bootloader. This tree has been built and
inspected, but the TFTP/USB paths still need validation on hardware.
