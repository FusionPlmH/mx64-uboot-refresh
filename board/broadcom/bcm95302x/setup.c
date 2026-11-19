#include <common.h>
#include <asm/io.h>
#include <version.h>

#define ICFG_CHIP_ID_REG		(0x18000000)
#define ChipcommonA_GPIOInput		(0x18000060)
#define ChipcommonA_GPIOOut		(0x18000064)
#define ChipcommonA_GPIOOutEn		(0x18000068)
#define CRU_GPIO_CONTROL0               (0x1803F1C0)
#define IPROC_IDM_DMAC_RESET_CONTROL	(0x18114800)
#define USB2_IDM_IDM_IO_CONTROL_DIRECT	(0x18115408)
#define IPROC_IDM_USB2_RESET_CONTROL	(0x18115800)
#define IPROC_STRAP_SKU_VECTOR		(0x1811A500)

extern int do_ccsetup(void);

int ehci_init(void)
{
	int usb2_clk_enable, usb2_reset_state;

	__raw_readl(IPROC_STRAP_SKU_VECTOR);
	usb2_clk_enable = __raw_readl(USB2_IDM_IDM_IO_CONTROL_DIRECT);
//	printf("Before reset, USB clk enable register is: %08x\n", usb2_clk_enable);
	usb2_clk_enable |= 0x1;
	__raw_writel( usb2_clk_enable, USB2_IDM_IDM_IO_CONTROL_DIRECT);
	usb2_clk_enable = __raw_readl(USB2_IDM_IDM_IO_CONTROL_DIRECT);
//	printf("Before reset, USB clk enable register is: %08x\n", usb2_clk_enable);
	udelay(1000);
	usb2_reset_state = __raw_readl(IPROC_IDM_USB2_RESET_CONTROL);
	if ((usb2_reset_state & 1) == 1)
	{
//		printf("\nBring usb2h_out of reset.......\n");
		__raw_writel(0x0, IPROC_IDM_USB2_RESET_CONTROL);
		udelay(1000);
		usb2_reset_state = __raw_readl(IPROC_IDM_USB2_RESET_CONTROL);
//		printf("usb2_reset_state is set and now it is: %08x\n", usb2_reset_state);
	};
	return 0;
};
/*
U_BOOT_CMD(
	ehciinit, 1, 0, ehci_init, "", ""
);
*/

int periph_enable(void)
{
	int tmp;

	/* enable orange pins, USB port, reset button & white led CRU PINs */
	tmp = __raw_readl(CRU_GPIO_CONTROL0);
	tmp |= ( 1 << 0 ); tmp |= ( 1 << 1 ); tmp |= ( 1 << 6 ); tmp |= ( 1 << 31 );
	__raw_writel(tmp, CRU_GPIO_CONTROL0);

	/* enable USB power */
	tmp = __raw_readl(ChipcommonA_GPIOOutEn);
	tmp |= ( 1 << 1 );
	__raw_writel(tmp, ChipcommonA_GPIOOutEn);

	/* Bring DMA out of reset */
	__raw_writel(0x0, IPROC_IDM_DMAC_RESET_CONTROL);

	return 0;
};
/*
int dma_init(void)
{
	return (__raw_writel(0x0, IPROC_IDM_DMAC_RESET_CONTROL));
};
*/
void led_flash(int time)
{
	uint32_t out;
	uint32_t outen;
	int i;

	out = __raw_readl(ChipcommonA_GPIOOut);
	outen = __raw_readl(ChipcommonA_GPIOOutEn);
	out ^= ( 1 << 31 ); outen ^= ( 1 << 0 );
	__raw_writel(out, ChipcommonA_GPIOOut);
	__raw_writel(outen, ChipcommonA_GPIOOutEn);

	for (i = 0; i < 6; i++) {
	
		out ^= ( 1 << 0 ); outen ^= ( 1 << 31 );
		__raw_writel(out, ChipcommonA_GPIOOut);
		__raw_writel(outen, ChipcommonA_GPIOOutEn);
		mdelay(time);
	};
};

int get_socrev(void)
{
	uint32_t regData = __raw_readl(ICFG_CHIP_ID_REG);
	uint32_t tmp = ((regData & 0x000F0000) >> 16);
	if (4 <= tmp)
		return 1;
	else
		return 0;
};

static void led_pulse(int n)
{
	uint32_t out = __raw_readl(ChipcommonA_GPIOOut);
	uint32_t outen = __raw_readl(ChipcommonA_GPIOOutEn);
	int i;

	for (i = 0; i < n; i++) {
		out ^= (1 << 0); outen ^= (1 << 31);
		__raw_writel(out, ChipcommonA_GPIOOut);
		__raw_writel(outen, ChipcommonA_GPIOOutEn);
		mdelay(150);

		out ^= (1 << 0); outen ^= (1 << 31);
		__raw_writel(out, ChipcommonA_GPIOOut);
		__raw_writel(outen, ChipcommonA_GPIOOutEn);
		mdelay(350);
	}
}

int usb_boot(void)
{
	const char *files[2];
	int i, j, bus, ret = 1;

	files[0] = getenv("initramfs_filename");
	files[1] = getenv("initramfs_filename_a0");

	printf("USB: start (%s)\n", get_socrev() ? "B0+" : "A0");
	led_flash(250);

	for (i = 0; i < 3 && ret; i++) {
		printf("USB: attempt %d\n", i + 1);

		if (run_command("usb reset", 0)) {
			printf("USB: reset failed (code 1)\n");
			led_pulse(1);
			mdelay(500);
			continue;
		}

		for (bus = 0; bus < 2 && ret; bus++) {
			for (j = 0; j < 2; j++) {
				char cmd[256];

				if (!files[j] || !files[j][0])
					continue;

				snprintf(cmd, sizeof(cmd),
					 "fatload usb %d:1 ${loadaddr} %s",
					 bus, files[j]);
				printf("USB: fatload usb %d:1 %s\n", bus, files[j]);
				if (run_command(cmd, 0) == 0) {
					ret = 0;
					break;
				}
				printf("USB: fatload failed (code 2)\n");
			}
		}

		if (ret) {
			led_pulse(2);
			mdelay(500);
		}
	}

	if (ret) {
		printf("USB: giving up\n");
		return 1;
	}

	printf("USB: bootm\n");
	run_command("bootm ${loadaddr}#${config_dts}", 0);
	printf("USB: bootm failed (code 3)\n");
	led_pulse(3);
	return 1;
};

int rst_check(void)
{
	uint32_t regData;
	uint32_t tmp;
	tmp =  __raw_readl(ChipcommonA_GPIOInput);
	tmp = tmp & (1 << 6);
	return tmp;
}

int dev_init(void)
{
	do_ccsetup();
	periph_enable();
	ehci_init();

	printf("MX64 U-Boot %s (%s)\n", UBOOT_TAG,
	       get_socrev() ? "B0+" : "A0");

	if (rst_check() == 0) {
		printf("\nRESET pressed: USB boot enabled\n");
		usb_boot();
	}
	return 0;
}

U_BOOT_CMD(
	mx64init, 1, 0, dev_init, "", ""
);

U_BOOT_CMD(
	usbload, 1, 0, usb_boot, "",""
);

U_BOOT_CMD(
        getsocrev, 1, 0, get_socrev, "",""
);

static const char *env_str(const char *name)
{
	const char *val = getenv(name);

	return val ? val : "(unset)";
}

static int do_mx64info(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	printf("U-Boot   : %s\n", version_string);
	printf("Tag      : %s\n", UBOOT_TAG);
	printf("SoC      : %s\n", get_socrev() ? "B0+" : "A0");
	printf("Reset    : %d\n", rst_check() == 0);
	printf("USB boot : %s\n", env_str("enable_usb_boot"));
	printf("USB file : %s\n", env_str("initramfs_filename"));
	printf("USB A0   : %s\n", env_str("initramfs_filename_a0"));
	printf("loadaddr : %s\n", env_str("loadaddr"));
	printf("dts cfg  : %s\n", env_str("config_dts"));
	printf("ipaddr   : %s\n", env_str("ipaddr"));
	printf("serverip : %s\n", env_str("serverip"));
	printf("ethaddr  : %s\n", env_str("ethaddr"));
	printf("gmac     : %s\n", env_str("gmac"));
	printf("tftp file: %s\n", env_str("tftp_file"));
	return 0;
}

U_BOOT_CMD(
	mx64info, 1, 0, do_mx64info, "print MX64 boot information", ""
);
