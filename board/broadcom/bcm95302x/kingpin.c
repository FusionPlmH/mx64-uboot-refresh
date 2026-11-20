#include <common.h>
#include <configs/iproc_board.h>
#include <iproc_gpio.h>
#include <iproc_gpio_cfg.h>
#include <search.h>

/* First word of a loaded image as read by this little endian CPU.  Note that
 * the magic numbers below are stored big endian in the file, so the value a
 * 32-bit load returns is byte reversed.
 */
#define LOADER_MAGIC		0x4d495053	/* ASCII "SPIM" */
#define UIMAGE_MAGIC		0x56190527	/* bytes 27 05 19 56 */
#define FIT_MAGIC		0xedfe0dd0	/* bytes d0 0d fe ed */
#define MAX_IMAGE_SIZE		(128 << 20)	/* sanity limit for one image */

static struct tag *params;
static ulong atags_addr = 0x100;

static void setup_start_tag ()
{
   params = (struct tag *)atags_addr;;

   params->hdr.tag = ATAG_CORE;
   params->hdr.size = tag_size (tag_core);

   params->u.core.flags = 0;
   params->u.core.pagesize = 1 << 12;
   params->u.core.rootdev = 0;

   params = tag_next (params);
}

static void setup_end_tag()
{
	params->hdr.tag = ATAG_NONE;
	params->hdr.size = 0;
}

static void setup_commandline_tag(char *commandline)
{
   char *p;

   if (!commandline)
      return;

   /* eat leading white space */
   for (p = commandline; *p == ' '; p++);

   /* skip non-existent command lines so the kernel will still
    * use its default command line.
    */
   if (*p == '\0')
      return;

   params->hdr.tag = ATAG_CMDLINE;
   params->hdr.size =
      (sizeof (struct tag_header) + strlen (p) + 1 + 4) >> 2;

   strcpy (params->u.cmdline.cmdline, p);

   params = tag_next (params);
}

struct meraki_image_header {
    u_int32_t magic;    /* Every sufficiently advanced technology... */
    u_int32_t load_addr;   /* Address to load binary */
    u_int32_t size;     /* Size of binary */
    u_int32_t entry;    /* Entry point */
    u_int32_t crc;      /* CRC32 of image */
    u_int32_t reserved[3]; /* Reserved */
};

int meraki_bootbk(ulong addr, const char *part)
{
   ulong hdr_crc, calc_crc, length;
   struct meraki_image_header *hdr;
   char cmdline[128];
   void (*kernel_entry)(int zero, int arch, uint params);
   unsigned long r2=0;
   unsigned long machid;

   hdr = (struct meraki_image_header *)addr;

   /* Validate arguments */
   if (hdr->magic != LOADER_MAGIC) {
      puts("Bad Bootkernel magic\n");
      return CMD_RET_FAILURE;
   }

   if (hdr->size == 0 || hdr->size > MAX_IMAGE_SIZE) {
      printf("Bad Bootkernel size 0x%x\n", hdr->size);
      return CMD_RET_FAILURE;
   }

   /* save crc and then zero it out */
   hdr_crc = hdr->crc;
   hdr->crc = 0;

   /* recalculate image crc */
   length = hdr->size + sizeof(struct meraki_image_header);
   calc_crc = crc32_wd (0, (const uchar *) addr, length, CHUNKSZ_CRC32);

   /* restore the field so the image in RAM stays byte exact and bootbk
    * can be retried on the same buffer
    */
   hdr->crc = hdr_crc;

   /* match?? */
   if (calc_crc != hdr_crc) {
      puts("Bad CRC\n");
      printf("Len 0x%lx calc 0x%lx hdr 0x%lx addr 0x%lx\n",
	     length, calc_crc, hdr_crc, addr);
      return CMD_RET_FAILURE;
   }

   addr += sizeof(struct meraki_image_header);

   /* Pass kernel command line using ATAGS */
   setup_start_tag();
   snprintf(cmdline, sizeof(cmdline), "loader=u-boot part=%s", part);
   setup_commandline_tag(cmdline);
   setup_end_tag();

   printf("Starting bootkernel 0x%lx (part %s)\n", addr, part);
   kernel_entry = (void (*)(int, int, uint))addr;
   machid = CONFIG_MACH_TYPE;
   r2 = atags_addr;
   kernel_entry(0, machid, r2);

   return CMD_RET_SUCCESS;
}

int do_bootbk (cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
   if (argc != 3)
      return CMD_RET_USAGE;

   return meraki_bootbk(simple_strtoul (argv[1], NULL, 16), argv[2]);
}

/*
 * Boot whatever image sits at @addr.  OpenWrt ships FIT images which are
 * booted with bootm, while the Meraki transition firmware is a "SPIM" loader
 * image that is linked for the load address stored in its own header and has
 * to be entered through bootbk.
 */
int mx64_boot_image(ulong addr, const char *part)
{
   struct meraki_image_header *hdr;
   uint32_t magic = *(volatile uint32_t *)addr;
   const char *cfg;
   char cmd[64];
   ulong target;

   switch (magic) {
   case FIT_MAGIC:
   case UIMAGE_MAGIC:
      /* only FIT images take a configuration node name */
      cfg = (magic == FIT_MAGIC) ? getenv("config_dts") : NULL;
      snprintf(cmd, sizeof(cmd), "bootm %08lx%s%s", addr,
	       (cfg && cfg[0]) ? "#" : "", (cfg && cfg[0]) ? cfg : "");
      printf("%s\n", cmd);
      return run_command(cmd, 0);

   case LOADER_MAGIC:
      hdr = (struct meraki_image_header *)addr;
      if (hdr->size == 0 || hdr->size > MAX_IMAGE_SIZE) {
	 printf("Bad bootkernel size 0x%x\n", hdr->size);
	 return CMD_RET_FAILURE;
      }

      target = hdr->load_addr;
      if (target != addr) {
	 /* the loader jumps to an absolute entry point, so it has to run
	  * from the address it was linked for
	  */
	 printf("Relocating to 0x%lx (payload 0x%x)\n", target, hdr->size);
	 memmove((void *)target, (void *)addr, sizeof(*hdr) + hdr->size);
      }

      return meraki_bootbk(target, part);

   default:
      printf("No FIT/uImage/SPIM magic at 0x%lx (%02x %02x %02x %02x)\n",
	     addr, ((uchar *)addr)[0], ((uchar *)addr)[1],
	     ((uchar *)addr)[2], ((uchar *)addr)[3]);
   }

   return CMD_RET_FAILURE;
}

static int do_bootimg(cmd_tbl_t *cmdtp, int flag, int argc,
		      char * const argv[])
{
   if (argc < 2 || argc > 3)
      return CMD_RET_USAGE;

   return mx64_boot_image(simple_strtoul(argv[1], NULL, 16),
			  argc == 3 ? argv[2] : "bootkernel1");
}

U_BOOT_CMD(
	bootbk, 3, 0, do_bootbk,
	"Boot Meraki bootkernel",
	"bootbk <address> <partition-name>"
);

U_BOOT_CMD(
	bootimg, 3, 0, do_bootimg,
	"Boot a FIT/uImage or a Meraki SPIM bootkernel image",
	"bootimg <address> [partition-name]"
);

#ifdef CONFIG_CMD_NAND
/*
 * Rewrite the boot loader from a file fetched over TFTP, then read it back and
 * compare CRCs.  The partition size has to be given explicitly: the erase
 * covers the whole partition, and on units where mtd0 is 0x80000 the next
 * partition (shmoo) starts right behind it.
 */
static int do_flash_uboot(cmd_tbl_t *cmdtp, int flag, int argc,
			  char * const argv[])
{
	const char *env;
	ulong part_size, load, verify, len;
	uint32_t crc_ram, crc_nand;
	char cmd[192];

	if (argc < 3 || strcmp(argv[argc - 1], "confirm") != 0) {
		printf("\nThis erases and rewrites the boot loader.\n"
		       "  flash_uboot <tftp-file> <partition-size-hex> confirm\n"
		       "Take the size from 'cat /proc/mtd' (mtd0) first.\n");
		return CMD_RET_USAGE;
	}

	part_size = simple_strtoul(argv[2], NULL, 16);
	if (part_size < 0x40000 || part_size > 0x200000 ||
	    (part_size & 0x3ffff)) {
		printf("Bad partition size 0x%lx (0x40000..0x200000, aligned)\n",
		       part_size);
		return CMD_RET_FAILURE;
	}

	env = getenv("loadaddr");
	load = env ? simple_strtoul(env, NULL, 16) : CONFIG_LOADADDR;
	env = getenv("verifyaddr");
	verify = env ? simple_strtoul(env, NULL, 16) : 0x91000000;

	if (!getenv("ipaddr"))
		setenv("ipaddr", "192.168.1.10");
	if (!getenv("serverip"))
		setenv("serverip", "192.168.1.2");
	setenv("netretry", "no");

	snprintf(cmd, sizeof(cmd), "tftpboot %08lx %s", load, argv[1]);
	printf("= %s\n", cmd);
	if (run_command(cmd, 0)) {
		printf("TFTP failed, nothing was written\n");
		return CMD_RET_FAILURE;
	}

	env = getenv("filesize");
	len = env ? simple_strtoul(env, NULL, 16) : 0;
	if (len < 0x10000 || len > part_size ||
	    (*(volatile uint32_t *)load & 0x0e000000) != 0x0a000000) {
		printf("Refusing 0x%lx bytes at 0x%lx (first word 0x%08x): "
		       "not an ARM U-Boot image that fits 0x%lx\n",
		       len, load, *(volatile uint32_t *)load, part_size);
		return CMD_RET_FAILURE;
	}

	crc_ram = crc32_wd(0, (const uchar *)load, len, CHUNKSZ_CRC32);
	printf("Writing 0x%lx bytes, erasing 0x%lx at NAND offset 0\n",
	       len, part_size);

	snprintf(cmd, sizeof(cmd), "nand erase 0 0x%lx", part_size);
	if (run_command(cmd, 0)) {
		printf("erase failed, the old boot loader is gone\n");
		return CMD_RET_FAILURE;
	}

	snprintf(cmd, sizeof(cmd), "nand write 0x%lx 0 0x%lx", load, len);
	if (run_command(cmd, 0)) {
		printf("write failed, the boot loader is damaged\n");
		return CMD_RET_FAILURE;
	}

	snprintf(cmd, sizeof(cmd), "nand read 0x%lx 0 0x%lx", verify, len);
	if (run_command(cmd, 0)) {
		printf("read back failed, cannot verify\n");
		return CMD_RET_FAILURE;
	}

	crc_nand = crc32_wd(0, (const uchar *)verify, len, CHUNKSZ_CRC32);
	if (crc_nand != crc_ram) {
		printf("VERIFY FAILED ram 0x%08x nand 0x%08x - write it again\n",
		       crc_ram, crc_nand);
		return CMD_RET_FAILURE;
	}

	printf("OK: 0x%lx bytes written and verified (crc 0x%08x)\n",
	       len, crc_ram);
	printf("Run 'reset' to start the new boot loader.\n");

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	flash_uboot, 4, 0, do_flash_uboot,
	"rewrite the boot loader from a TFTP file and verify it",
	"<tftp-file> <partition-size-hex> confirm"
);
#endif /* CONFIG_CMD_NAND */

/* The MX64 has no RGB LED: the amber/white front LEDs are driven by
 * board/broadcom/bcm95302x/setup.c (led_init/led_flash/led_pulse).
 */
#ifndef CONFIG_CMD_NET
void
NetReceive(uchar *inpkt, int len)
{
}
uchar *NetRxPackets[4];
int eth_getenv_enetaddr(char *name, uchar *enetaddr)
{
   return 0;
}

int eth_register(struct eth_device *dev)
{
   return 0;
}
#endif /* !CONFIG_CMD_NET */
/*
char *getenv(const char *name)
{ return NULL; }

ulong getenv_ulong(const char *name, int base, ulong default_val)
{ return default_val; }

int setenv(const char *varname, const char *varvalue)
{ return 1; }

int env_check_apply(const char *name, const char *oldval,
			const char *newval, int flag)
{ return 1; }
*/
//ulong load_addr = CONFIG_SYS_LOAD_ADDR;	/* Default Load Address */

//bint himport_r(struct hsearch_data *htab,
//		const char *env, size_t size, const char sep, int flag,
//		int nvars, char * const vars[], int do_apply)
//{ return 0; }
