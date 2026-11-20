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

/* Front panel LEDs, see bcm958625-meraki-mx6x-common.dtsi */
#define GPIO_LED_AMBER			0	/* power, active low */
#define GPIO_LED_WHITE			31	/* status, active high */

extern int do_ccsetup(void);
extern int mx64_boot_image(ulong addr, const char *part);

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
/* Amber is the power LED and stays on; the white status LED is used to
 * signal boot progress.  Both GPIOs are driven push-pull by the GPIO block.
 */
void led_init(void)
{
	uint32_t cru, out, outen;

	/* route GPIO 0/31 to the GPIO block (periph_enable() does this too,
	 * repeat it here so the LEDs also work from a bare command line)
	 */
	cru = __raw_readl(CRU_GPIO_CONTROL0);
	cru |= (1U << GPIO_LED_AMBER) | (1U << GPIO_LED_WHITE);
	__raw_writel(cru, CRU_GPIO_CONTROL0);

	outen = __raw_readl(ChipcommonA_GPIOOutEn);
	outen |= (1U << GPIO_LED_AMBER) | (1U << GPIO_LED_WHITE);
	__raw_writel(outen, ChipcommonA_GPIOOutEn);

	/* amber on (active low), white off */
	out = __raw_readl(ChipcommonA_GPIOOut);
	out &= ~(1U << GPIO_LED_AMBER);
	out &= ~(1U << GPIO_LED_WHITE);
	__raw_writel(out, ChipcommonA_GPIOOut);
}

static void led_status(int on)
{
	uint32_t out = __raw_readl(ChipcommonA_GPIOOut);

	if (on)
		out |= (1U << GPIO_LED_WHITE);
	else
		out &= ~(1U << GPIO_LED_WHITE);
	__raw_writel(out, ChipcommonA_GPIOOut);
}

void led_flash(int time)
{
	int i;

	led_init();
	for (i = 0; i < 3; i++) {
		led_status(1);
		mdelay(time);
		led_status(0);
		mdelay(time);
	}
}

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
	int i;

	for (i = 0; i < n; i++) {
		led_status(1);
		mdelay(150);
		led_status(0);
		mdelay(350);
	}
}

int usb_boot(void)
{
	const char *files[2];
	const char *load;
	ulong addr;
	char cmd[192];
	int attempt, bus, j, loaded;

	files[0] = getenv("initramfs_filename");
	files[1] = getenv("initramfs_filename_a0");

	load = getenv("loadaddr");
	addr = load ? simple_strtoul(load, NULL, 16) : CONFIG_LOADADDR;

	printf("USB: start (%s)\n", get_socrev() ? "B0+" : "A0");
	led_flash(250);

	for (attempt = 0; attempt < 3; attempt++) {
		printf("USB: attempt %d\n", attempt + 1);

		/* "usb reset" reports success even when the host controller
		 * stayed dead, so ask the storage layer instead of trusting
		 * its return code
		 */
		run_command("usb reset", 0);
		if (!usb_stor_get_dev(0)) {
			printf("USB: no storage device\n");
			led_pulse(1);
			mdelay(500);
			continue;
		}

		run_command("usb info", 0);

		loaded = 0;
		for (bus = 0; bus < 2; bus++) {
			for (j = 0; j < 2; j++) {
				if (!files[j] || !files[j][0])
					continue;

				snprintf(cmd, sizeof(cmd),
					 "fatload usb %d:1 %08lx %s",
					 bus, addr, files[j]);
				printf("USB: %s\n", cmd);
				if (run_command(cmd, 0))
					continue;

				loaded = 1;

				/* a loaded image that does not boot must not
				 * stop us from trying the remaining candidates
				 */
				printf("USB: booting %s\n", files[j]);
				mx64_boot_image(addr, "bootkernel1");
				printf("USB: %s did not boot\n", files[j]);
			}
		}

		if (loaded)
			led_pulse(3);
		else
			led_pulse(2);
		mdelay(500);
	}

	printf("USB: giving up\n");
	return 1;
}

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

static int do_mx64init(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	return dev_init() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_usb_boot(cmd_tbl_t *cmdtp, int flag, int argc,
		       char * const argv[])
{
	return usb_boot() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_get_socrev(cmd_tbl_t *cmdtp, int flag, int argc,
			 char * const argv[])
{
	int b0 = get_socrev();

	/*
	 * U-Boot's do_test() returns 1 (false) when it gets fewer than three
	 * arguments, and hush drops an empty quoted word, so
	 *
	 *     if test -z "${config_dts}"; then getsocrev set; fi
	 *
	 * collapsed to "test -z" and never ran the body. Call this
	 * unconditionally from soc_dtscheck instead, and skip only when the
	 * variable already has a real value.
	 */
	if (argc >= 2 && !strcmp(argv[1], "set")) {
		const char *cur = getenv("config_dts");

		if (cur && *cur)
			return 0;

		setenv("config_dts", b0 ? "config@4" : "config@2");
		printf("SoC: %s -> %s\n", b0 ? "B0+" : "A0",
		       b0 ? "config@4" : "config@2");
		return 0;
	}

	return b0;
}

U_BOOT_CMD(
	mx64init, 1, 0, do_mx64init,
	"init MX64 peripherals and check the reset button",
	""
);

U_BOOT_CMD(
	usbload, 1, 0, do_usb_boot,
	"load an initramfs/transition image from USB and boot it",
	""
);

U_BOOT_CMD(
	getsocrev, 2, 0, do_get_socrev,
	"return 1 on B0+ silicon, 0 on A0; 'getsocrev set' sets config_dts",
	"getsocrev [set]\n"
	"    - no arg: exit code only (1 = B0+, 0 = A0)\n"
	"    - set   : set env config_dts to config@4 (B0+) or config@2 (A0)"
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
