/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <app.h>
#include <debug.h>
#include <arch/arm.h>
#include <dev/udc.h>
#include <string.h>
#include <kernel/thread.h>
#include <arch/ops.h>
#include <version.h>

#include <dev/flash.h>
#include <lib/ptable.h>
#include <dev/keys.h>
#include <dev/fbcon.h>

#include "aboot.h"
#include "bootimg.h"
#include "fastboot.h"
#include "recovery.h"
#include "menu.h"

#define EXPAND(NAME) #NAME
#define TARGET(NAME) EXPAND(NAME)
#define DEFAULT_CMDLINE "";

#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500

//static const char *emmc_cmdline = " androidboot.emmc=";
static const char *battchg_pause = " androidboot.mode=offmode_charging";


static struct udc_device surf_udc_device = {
	.vendor_id	= 0x18d1,
	.product_id	= 0xD00D,
	.version_id	= 0x0100,
	.manufacturer	= "Google",
	.product	= "Android",
	.serialno    = "HT9000000000"
};

struct atag_ptbl_entry
{
	char name[16];
	unsigned offset;
	unsigned size;
	unsigned flags;
};

void platform_uninit_timer(void);
unsigned* target_atag_mem(unsigned* ptr);
unsigned board_machtype(void);
unsigned check_reboot_mode(void);
void *target_get_scratch_address(void);
int target_is_emmc_boot(void);
void reboot_device(unsigned);
void target_battery_charging_enable(unsigned enable, unsigned disconnect);
void display_shutdown(void);

static void ptentry_to_tag(unsigned **ptr, struct ptentry *ptn)
{
	struct atag_ptbl_entry atag_ptn;

	if (ptn->type == TYPE_MODEM_PARTITION)
		return;
	memcpy(atag_ptn.name, ptn->name, 16);
	atag_ptn.name[15] = '\0';
	atag_ptn.offset = ptn->start;
	atag_ptn.size = ptn->length;
	atag_ptn.flags = ptn->flags;
	memcpy(*ptr, &atag_ptn, sizeof(struct atag_ptbl_entry));
	*ptr += sizeof(struct atag_ptbl_entry) / sizeof(unsigned);
}

void boot_linux(void *kernel, unsigned *tags, 
		const char *cmdline, unsigned machtype,
		void *ramdisk, unsigned ramdisk_size)
{
	unsigned *ptr = tags;
	unsigned pcount = 0;
	void (*entry)(unsigned,unsigned,unsigned*) = kernel;
	struct ptable *ptable;
	int cmdline_len = 0;
	int have_cmdline = 0;
	int pause_at_bootup = 0;

	/* CORE */
	*ptr++ = 2;
	*ptr++ = 0x54410001;

	if (ramdisk_size) {
		*ptr++ = 4;
		*ptr++ = 0x54420005;
		*ptr++ = (unsigned)ramdisk;
		*ptr++ = ramdisk_size;
	}

	ptr = target_atag_mem(ptr);

	if ((ptable = flash_get_ptable()) && (ptable->count != 0)) {
		int i;
		for(i=0; i < ptable->count; i++) {
			struct ptentry *ptn;
			ptn =  ptable_get(ptable, i);
			if (ptn->type == TYPE_APPS_PARTITION)
				pcount++;
		}
		*ptr++ = 2 + (pcount * (sizeof(struct atag_ptbl_entry) /
					       sizeof(unsigned)));
		*ptr++ = 0x4d534d70;
		for (i = 0; i < ptable->count; ++i)
			ptentry_to_tag(&ptr, ptable_get(ptable, i));
	}
	
	if (cmdline && cmdline[0]) {
		cmdline_len = strlen(cmdline);
		have_cmdline = 1;
	}
	if (target_pause_for_battery_charge()) {
		pause_at_bootup = 1;
		cmdline_len += strlen(battchg_pause);
	}
	if (cmdline_len > 0) {
		const char *src;
		char *dst;
		unsigned n;
		/* include terminating 0 and round up to a word multiple */
		n = (cmdline_len + 4) & (~3);
		*ptr++ = (n / 4) + 2;
		*ptr++ = 0x54410009;
		dst = (char *)ptr;
		if (have_cmdline) {
			src = cmdline;
			while ((*dst++ = *src++));
		}
		if (pause_at_bootup) {
			src = battchg_pause;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
		ptr += (n / 4);
	}

	/* END */
	*ptr++ = 0;
	*ptr++ = 0;

	dprintf(INFO, "booting linux @ %p, ramdisk @ %p (%d)\n",
		kernel, ramdisk, ramdisk_size);
	if (cmdline)
		dprintf(INFO, "cmdline: %s\n", cmdline);

	enter_critical_section();
	platform_uninit_timer();
	arch_disable_cache(UCACHE);
	arch_disable_mmu();

	htcleo_boot(kernel, machtype, tags);
	//entry(0, machtype, tags);
}

unsigned page_size = 0;
unsigned page_mask = 0;

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

static unsigned char buf[4096]; //Equal to max-supported pagesize

int boot_linux_from_flash(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	unsigned n;
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned offset = 0;
	const char *cmdline;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return -1;
	}

	if(!boot_into_recovery)
	{
	        ptn = ptable_find(ptable, "boot");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No boot partition found\n");
		        return -1;
	        }
	}
	else
	{
	        ptn = ptable_find(ptable, "recovery");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No recovery partition found\n");
		        return -1;
	        }
	}

	if (flash_read(ptn, offset, buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
		return -1;
	}
	offset += page_size;

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invaled boot image heador\n");
		return -1;
	}

	if (hdr->page_size != page_size) {
		dprintf(CRITICAL, "ERROR: Invaled boot image pagesize. Device pagesize: %d, Image pagesize: %d\n",page_size,hdr->page_size);
		return -1;
	}

	n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
	if (flash_read(ptn, offset, (void *)hdr->kernel_addr, n)) {
		dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
		return -1;
	}
	offset += n;

	n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
	if (flash_read(ptn, offset, (void *)hdr->ramdisk_addr, n)) {
		dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
		return -1;
	}
	offset += n;

continue_boot:
	dprintf(INFO, "\nkernel  @ %x (%d bytes)\n", hdr->kernel_addr,
		hdr->kernel_size);
	dprintf(INFO, "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr,
		hdr->ramdisk_size);

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	strcat(cmdline," clk=");
	strcat(cmdline,cLK_version);

	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	/* TODO: create/pass atags to kernel */

	dprintf(INFO, "\nBooting Linux\n");
	boot_linux((void *)hdr->kernel_addr, (void *)TAGS_ADDR,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	static struct boot_img_hdr hdr;
	char *ptr = ((char*) data);

	if (sz < sizeof(hdr)) {
		fastboot_fail("invalid bootimage header");
		return;
	}

	memcpy(&hdr, data, sizeof(hdr));

	/* ensure commandline is terminated */
	hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

	kernel_actual = ROUND_TO_PAGE(hdr.kernel_size, page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr.ramdisk_size, page_mask);

	//cedesmith: this will prevent lk booting lk.bin 
	//if (page_size + kernel_actual + ramdisk_actual < sz) {
	//	fastboot_fail("incomplete bootimage");
	//	return;
	//}

	memmove((void*) KERNEL_ADDR, ptr + page_size, hdr.kernel_size);
	memmove((void*) RAMDISK_ADDR, ptr + page_size + kernel_actual, hdr.ramdisk_size);

	fastboot_okay("");
	target_battery_charging_enable(0, 1);
	udc_stop();

	boot_linux((void*) KERNEL_ADDR, (void*) TAGS_ADDR,
		   (const char*) hdr.cmdline, board_machtype(),
		   (void*) RAMDISK_ADDR, hdr.ramdisk_size);
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (flash_erase(ptn)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}




void cmd_flash(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned extra = 0;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (!strcmp(ptn->name, "boot") || !strcmp(ptn->name, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(ptn->name, "system") || !strcmp(ptn->name, "userdata")
	    || !strcmp(ptn->name, "persist"))
		extra = ((page_size >> 9) * 16);
	else
		sz = ROUND_TO_PAGE(sz, page_mask);

	dprintf(INFO, "writing %d bytes to '%s'\n", sz, ptn->name);
	if (flash_write(ptn, extra, data, sz)) {
		fastboot_fail("flash write failure");
		return;
	}
	dprintf(INFO, "partition '%s' updated\n", ptn->name);
	fastboot_okay("");
}

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	target_battery_charging_enable(0, 1);
	udc_stop();
	boot_linux_from_flash();
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(0);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(FASTBOOT_MODE);
}

void aboot_init(const struct app_descriptor *app)
{
	page_size = flash_page_size();
	page_mask = page_size - 1;
	
	/* Check if we should do something other than booting up */
	if (keys_get_state(KEY_HOME) != 0)
		boot_into_recovery = 1;
	if (keys_get_state(KEY_BACK) != 0)
		goto menu;

	unsigned reboot_mode = check_reboot_mode();
	if (reboot_mode == ANDRBOOT_MODE) {
		// DO NOTHING
	} else if (reboot_mode == RECOVERY_MODE) {
		boot_into_recovery = 1;
	} else if (reboot_mode == FASTBOOT_MODE) {
		goto menu;
	} else if (reboot_mode == DEV_FAC_RESET) {
		if(fbcon_display()==NULL) display_init();
		dprintf(INFO, "FACTORY RESET DEVICE IN 5 SECONDS.\n");
		dprintf(INFO, "TO STOP, KEEP HOME KEY PRESSED.\n");
		for( unsigned counter = 0; counter < 125; counter++)
		{
			if(keys_get_state(KEY_HOME) != 0) reboot_device(ANDRBOOT_MODE);
			thread_sleep(5);
		}
		flash_erase("cache");
		flash_erase("userdata");
		dprintf(INFO, "FACTORY RESET COMPLETE.\n");
		mdelay(5);
		reboot_device(ANDRBOOT_MODE);
	}

	recovery_init();
	boot_linux_from_flash();
menu:	{menu_init();}
}

APP_START(aboot)
	.init = aboot_init,
APP_END

