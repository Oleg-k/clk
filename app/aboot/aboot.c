/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Copyright (c) 2011, Rick@xda-developers.com, All rights reserved.
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
#include <arch/arm.h>
#include <arch/ops.h>
#include <kernel/thread.h>
#include <platform/iomap.h>
#include <platform/timer.h>
#include <sys/types.h>
 
#include <app.h>
#include <bits.h>
#include <compiler.h>
#include <debug.h>
#include <err.h>
#include <hsusb.h>
#include <platform.h>
#include <reg.h>
#include <smem.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <dev/flash.h>
#include <dev/fbcon.h>
#include <dev/keys.h>
#include <dev/udc.h>

#include <lib/ptable.h>
#include <lib/vptable.h>

#include "bootimg.h"
#include "fastboot.h"
#include "recovery.h"
#include "version.h"

#define EXPAND(NAME) #NAME
#define TARGET(NAME) EXPAND(NAME)
#define DEFAULT_CMDLINE "";

#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500

#define ERR_KEY_CHANGED 99

void platform_uninit_timer(void);
void *target_get_scratch_address(void);
void reboot_device(unsigned reboot_reason);
void target_battery_charging_enable(unsigned enable, unsigned disconnect);
void display_shutdown(void);
void nand_erase(const char *arg);
void cmd_powerdown(const char *arg, void *data, unsigned sz);
void disp_menu(int selection, char *menu[], int loop);
void shutdown(void);
void keypad_init(void);
void display_init(void);
void htcleo_ptable_dump(struct ptable *ptable);
void cmd_dmesg(const char *arg, void *data, unsigned sz);
void target_display_init();
void cmd_oem_register();
void shutdown_device(void);
void dispmid(const char *fmt, int sel);
void start_keylistener(void);
void eval_keyup(void);
void eval_keydown(void);
void draw_clk_header(void);
void cmd_flashlight(void);
void redraw_menu(void);
void prnt_nand_stat(void);

int boot_linux_from_flash(void);

int key_listener(void *arg);

unsigned boot_into_sboot = 0;
unsigned board_machtype(void);
unsigned get_boot_reason(void);
unsigned check_reboot_mode(void);
unsigned* target_atag_mem(unsigned* ptr);

uint16_t keys[] = { KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_SOFT1, KEY_SEND, KEY_CLEAR, KEY_BACK, KEY_HOME };
uint16_t keyp   = ERR_KEY_CHANGED;

static const char *battchg_pause = " androidboot.mode=offmode_charging";

static unsigned char buf[4096]; //Equal to max-supported pagesize

char spl_version[24];
char splBuffer[24];
char radio_version[25];
char radBuffer[25];

struct atag_ptbl_entry
{
	char name[16];
	unsigned offset;
	unsigned size;
	unsigned flags;
};

struct menu_item 
{
	char mTitle[64];
	char command[64];
	
	int x;
	int y;
};

#define MAX_MENU 16

struct menu 
{
	int maxarl;
	int selectedi;
	int goback;
	
	char MenuId[80];
	char backCommand[64];
	char data[64];

	struct menu_item item[MAX_MENU];
};

struct menu main_menu;
struct menu sett_menu;
struct menu rept_menu;
struct menu cust_menu;
struct menu *active_menu;

static struct udc_device surf_udc_device = 
{
	.vendor_id	= 0x18d1,
	.product_id	= 0x0D02,
	.version_id	= 0x0001,
	.manufacturer	= "Google",
	.product	= "Android",
};

char charVal(char c)
{
	c&=0xf;
	if(c<=9) return '0'+c;
	return 'A'+(c-10);
}

int str2u(const char *x)
{
	while(*x==' ')x++;

	unsigned base=10;
	int sign=1;
    unsigned n = 0;

    if(strstr(x,"-")==x) { sign=-1; x++;};
    if(strstr(x,"0x")==x) {base=16; x+=2;}

    while(*x) 
	{
    	char d=0;
        switch(*x) {
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            d = *x - '0';
            break;
        case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            d = (*x - 'a' + 10);
            break;
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            d = (*x - 'A' + 10);
            break;
        default:
            return sign*n;
    	}
        if(d>=base) return sign*n;
        n*=base;n+=d;
        x++;
	}

    return sign*n;
}

/* koko : Method to get radio version */
void get_radio_ver(char* start, int len)
{
	while(len>0)
	{
		int slen = len > 29 ? 29 : len;
		for(int i=0; i<slen; i++)
		{
			radBuffer[i*2] = charVal(start[i]>>4);
			radBuffer[i*2+1]= charVal(start[i]);
		}
		radBuffer[slen*2+1]=0;
		start+=slen;
		len-=slen;
	}
}

void update_radio_ver(void)
{
	get_radio_ver((char*)str2u("0x1EF220"), str2u("0xA"));
	/* koko : if radio version is not read 1st char won't be '3' so return */
	char expected_byte[] = "3";
	if(radBuffer[0] != expected_byte[0]){return;}

	//hex2ansii
	char *ptrBuffer = (char*)&radio_version;
	for (int i = 0; i < (int)(strlen(radBuffer) - 1); i+=2)
	{
		int firstvalue = radBuffer[i] - '0';
		int secondvalue;
		switch(radBuffer[i+1])
		{
			case 'A': case 'a':
			{
				secondvalue = 10;
			}break;
			case 'B': case 'b':
			{
				secondvalue = 11;
			}break;
			case 'C': case 'c':
			{
				secondvalue = 12;
			}break;
			case 'D': case 'd':
			{
				secondvalue = 13;
			}break;
			case 'E': case 'e':
			{
				secondvalue = 14;
			}break;
			case 'F': case 'f':
			{
				secondvalue = 15;
			}break;
			default: 
				secondvalue = radBuffer[i+1] - '0';
				break;
		}
		int newval;
		newval = ((16 * firstvalue) + secondvalue);
		*ptrBuffer = (char)(newval);
		ptrBuffer++;
	}
	return;
}

/* koko : Method to get spl version */
void get_spl_ver(char* start, int len)
{
	while(len>0)
	{
		int slen = len > 29 ? 29 : len;
		for(int i=0; i<slen; i++)
		{
			splBuffer[i*2] = charVal(start[i]>>4);
			splBuffer[i*2+1]= charVal(start[i]);
		}
		splBuffer[slen*2+1]=0;
		start+=slen;
		len-=slen;
	}
}

void update_spl_ver(void)
{
	get_spl_ver((char*)str2u("0x1004"), str2u("0x9"));

	//hex2ansii
	char *ptrBuffer = (char*)&spl_version;
	for (int i = 0; i < (int)(strlen(splBuffer) - 1); i+=2)
	{
		int firstvalue = splBuffer[i] - '0';
		int secondvalue;
		switch(splBuffer[i+1])
		{
			case 'A': case 'a':
			{
				secondvalue = 10;
			}break;
			case 'B': case 'b':
			{
				secondvalue = 11;
			}break;
			case 'C': case 'c':
			{
				secondvalue = 12;
			}break;
			case 'D': case 'd':
			{
				secondvalue = 13;
			}break;
			case 'E': case 'e':
			{
				secondvalue = 14;
			}break;
			case 'F': case 'f':
			{
				secondvalue = 15;
			}break;
			default:
				secondvalue = splBuffer[i+1] - '0';
				break;
		}
		int newval;
		newval = ((16 * firstvalue) + secondvalue);
		*ptrBuffer = (char)newval;
		ptrBuffer++;
	}
	return;
}

void selector_disable(void)
{
	int sel_current_y_offset = fbcon_get_y_cord();
	int sel_current_x_offset = fbcon_get_x_cord();
	
	fbcon_set_x_cord(active_menu->item[active_menu->selectedi].x);
	fbcon_set_y_cord(active_menu->item[active_menu->selectedi].y);
	
	fbcon_forcetg(true);
	fbcon_reset_colors_rgb555();
	
	_dputs(active_menu->item[active_menu->selectedi].mTitle);
	
	fbcon_forcetg(false);
	fbcon_set_y_cord(sel_current_y_offset);
	fbcon_set_x_cord(sel_current_x_offset);
}

void selector_enable(void)
{
	int sel_current_y_offset = fbcon_get_y_cord();
	int sel_current_x_offset = fbcon_get_x_cord();
	
	fbcon_set_x_cord(active_menu->item[active_menu->selectedi].x);
	fbcon_set_y_cord(active_menu->item[active_menu->selectedi].y);
	
	fbcon_settg(0x001f);
	fbcon_setfg(0xffff);
	
	_dputs(active_menu->item[active_menu->selectedi].mTitle);
	
	fbcon_reset_colors_rgb555();
	fbcon_set_y_cord(sel_current_y_offset);
	fbcon_set_x_cord(sel_current_x_offset);
}

void add_menu_item(struct menu *xmenu,const char *name,const char *command);

void eval_command(void)
{
    char command[32];
	if (active_menu->goback)
	{
		active_menu->goback = 0;
		strcpy(command, active_menu->backCommand);
		if ( strlen( command ) == 0 )
			return;
	}
	else
		strcpy(command, active_menu->item[active_menu->selectedi].command);
	
	if (!memcmp(command,"boot_recv", strlen(command)))
	{
        fbcon_resetdisp();
        boot_into_sboot = 0;
        boot_into_recovery = 1;
        boot_linux_from_flash();
	}
	else if (!memcmp(command,"prnt_clrs", strlen(command)))
	{
	redraw_menu();
	}
	else if (!memcmp(command,"boot_sbot", strlen(command)))
	{
        fbcon_resetdisp();
        boot_into_sboot = 1;
        boot_into_recovery = 0;
        boot_linux_from_flash();
	}
	else if (!memcmp(command,"boot_nand", strlen(command)))
	{
        fbcon_resetdisp();
        boot_into_sboot = 0;
        boot_into_recovery = 0;
        boot_linux_from_flash();
	}
	else if (!memcmp(command,"prnt_stat", strlen(command)))
	{
		redraw_menu();
		vpart_list();
	}
	else if (!memcmp(command,"prnt_nand", strlen(command)))
	{
		redraw_menu();
		prnt_nand_stat();
	}
	else if (!memcmp(command,"goto_rept", strlen(command)))
	{
		active_menu = &rept_menu;
		redraw_menu();
	}
	else if (!memcmp(command,"goto_sett", strlen(command)))
	{
		active_menu = &sett_menu;
		redraw_menu();
	}
	else if (!memcmp(command,"goto_main", strlen(command)))
	{
		active_menu = &main_menu;
		redraw_menu();
	}
	else if (!memcmp(command,"acpu_ggwp", strlen(command)))
	{
        reboot_device(0);
	}
	else if (!memcmp(command,"acpu_bgwp", strlen(command)))
	{
		reboot_device(FASTBOOT_MODE);
	}
	else if (!memcmp(command,"acpu_pawn", strlen(command)))
	{
        shutdown();
	}
	else if (!memcmp(command,"enable_extrom", strlen(command)))
	{
		vpart_enable_extrom();
		printf("Will Auto-Reboot in 2 Seconds for changes to take place.");
		thread_sleep(2000);
		reboot_device(FASTBOOT_MODE);
	}
	else if (!memcmp(command,"disable_extrom", strlen(command)))
	{
		vpart_disable_extrom();
		printf("Will Auto-Reboot in 2 Seconds for changes to take place.");
		thread_sleep(2000);
		reboot_device(FASTBOOT_MODE);
	}
	else if (!memcmp(command,"init_flsh", strlen(command)))
	{
		cmd_flashlight();
	}
	else if (!memcmp(command, "rept_", 5 ))
	{
		char* subCommand = command + 5;
		if ( active_menu == &rept_menu )
		{
			if ( !memcmp( subCommand, "commit", strlen( subCommand ) ) )
			{
				cust_menu.selectedi		= 1;	// No by default
				cust_menu.maxarl		= 0;
				cust_menu.goback		= 0;

				sprintf( cust_menu.MenuId, "Commit changes to NAND?" );
				strcpy( cust_menu.backCommand, "goto_rept" );

				add_menu_item(&cust_menu, "YES"	, "rept_write");
				add_menu_item(&cust_menu, "NO"	, "goto_rept");

				active_menu = &cust_menu;

				redraw_menu();

				vpart_list();

				return;
			}
			else
			{
				cust_menu.selectedi     = 0;
				cust_menu.maxarl		= 0;
				cust_menu.goback		= 0;

				strcpy( cust_menu.data, subCommand );
				sprintf( cust_menu.MenuId, "%s = %d MB", cust_menu.data, (int) ( vpart_partition_size( cust_menu.data ) / get_blk_per_mb() ) );
				strcpy( cust_menu.backCommand, "goto_rept" );

				add_menu_item(&cust_menu, "+10"			, "rept_add_10");
				add_menu_item(&cust_menu, "+1"			, "rept_add_1");
				add_menu_item(&cust_menu, "-1"			, "rept_rem_1");
				add_menu_item(&cust_menu, "-10"			, "rept_rem_10");

				active_menu = &cust_menu;
			}
		}
		else if ( active_menu == &cust_menu )
		{
			if ( !memcmp( subCommand, "add_", 4 ) )
			{
				int add = atoi( subCommand + 4 );		// MB
				vpart_resize_ex( cust_menu.data, (int) vpart_partition_size( cust_menu.data ) / get_blk_per_mb() + add );
				sprintf( cust_menu.MenuId, "%s = %d MB", cust_menu.data, (int) ( vpart_partition_size( cust_menu.data ) / get_blk_per_mb() ) );
			}
			else if ( !memcmp( subCommand, "rem_", 4 ) )
			{
				int rem = atoi( subCommand + 4 );		// MB
				int size = (int) vpart_partition_size( cust_menu.data ) / get_blk_per_mb() - rem;

				// Avoid setting partition as variable
				if ( size > 0 )
				{
					vpart_resize_ex( cust_menu.data, size );
					sprintf( cust_menu.MenuId, "%s = %d MB", cust_menu.data, (int) ( vpart_partition_size( cust_menu.data ) / get_blk_per_mb() ) );
				}
			}
			else if ( !memcmp( subCommand, "write", strlen( subCommand ) ) )
			{
				// Apply changes
				vpart_resize_asize();
				vpart_commit();

				// Auto reboot to load new PTABLE layout
				printf("Will Auto-Reboot in 2 Seconds for changes to take place.");
				thread_sleep(2000);
				reboot_device(FASTBOOT_MODE);

				return;
			}
			else
			{
				printf("HBOOT BUG: Somehow fell through eval_cmd()\n");
				return;
			}
		}

		redraw_menu();
	}
	else
	{
		printf("HBOOT BUG: Somehow fell through eval_cmd()\n");
	}
}

void redraw_menu(void)
{
	fbcon_resetdisp();
	draw_clk_header();
	for (uint8_t i = 0 ;; i++)
	{
		if ((strlen(active_menu->item[i].mTitle) != 0) && !(i > active_menu->maxarl))
		{
			_dputs("   ");
			if(active_menu->item[i].x == 0)
				active_menu->item[i].x = fbcon_get_x_cord();
			if(active_menu->item[i].y == 0)
				active_menu->item[i].y = fbcon_get_y_cord();
			_dputs(active_menu->item[i].mTitle);
			_dputs("\n");
		} else break;
	}
	_dputs("\n");
	selector_enable();
}

static int menu_item_down()
{
	thread_set_priority(HIGHEST_PRIORITY);
	
	if (didyouscroll())
		redraw_menu();
	
	selector_disable();
	
	if (active_menu->selectedi == (active_menu->maxarl-1))
		active_menu->selectedi=0;
	else
		active_menu->selectedi++;
	
    selector_enable();
	
    thread_set_priority(DEFAULT_PRIORITY);
	return 0;
}

static int menu_item_up()
{
	thread_set_priority(HIGHEST_PRIORITY);
	
	if (didyouscroll())
		redraw_menu();

	selector_disable();
	
	if ((active_menu->selectedi) == 0)
		active_menu->selectedi=(active_menu->maxarl-1);
	else
		active_menu->selectedi--;
	
    selector_enable();
	
	thread_set_priority(DEFAULT_PRIORITY);
	return 0;
}

void add_menu_item(struct menu *xmenu,const char *name,const char *command)
{
	if(xmenu->maxarl==MAX_MENU)
	{
		printf("Menu: is overloaded with entry %s",name);
		return;
	}

	strcpy(xmenu->item[xmenu->maxarl].mTitle,name);
	strcpy(xmenu->item[xmenu->maxarl].command,command);
	
	xmenu->item[xmenu->maxarl].x = 0;
	xmenu->item[xmenu->maxarl].y = 0;
	
	xmenu->maxarl++;
	return;
}
void init_menu()
{
	main_menu.selectedi     = 0;
	main_menu.maxarl		= 0;
	main_menu.goback		= 0;

	strcpy( main_menu.MenuId, "HBOOT" );
	strcpy( main_menu.backCommand, "" );
	
	add_menu_item(&main_menu, "ANDROID BOOT"  , "boot_nand");
	add_menu_item(&main_menu, "ANDROID SBOOT"  , "boot_sbot");
	add_menu_item(&main_menu, "RECOVERY"      , "boot_recv");
	add_menu_item(&main_menu, "FLASHLIGHT"    , "init_flsh");
	add_menu_item(&main_menu, "SETTINGS"      , "goto_sett");
	add_menu_item(&main_menu, "REBOOT"        , "acpu_ggwp");
	add_menu_item(&main_menu, "REBOOT HBOOT"  , "acpu_bgwp");
	add_menu_item(&main_menu, "POWERDOWN"     , "acpu_pawn");
	
	sett_menu.selectedi     = 0;
	sett_menu.maxarl		= 0;
	sett_menu.goback		= 0;

	strcpy( sett_menu.MenuId, "SETTINGS" );
	strcpy( sett_menu.backCommand, "goto_main" );
	
	if (vparts.extrom_enabled)
		add_menu_item(&sett_menu,"ExtROM ENABLED, SELECT TO DISABLE !","disable_extrom");
	else
		add_menu_item(&sett_menu,"ExtROM DISABLED, SELECT TO ENABLE !","enable_extrom");
	
	add_menu_item(&sett_menu, "PRINT NAND STATS", "prnt_nand");
	add_menu_item(&sett_menu, "PRINT PART STATS", "prnt_stat");
	add_menu_item(&sett_menu, "REPARTITION NAND", "goto_rept");

	rept_menu.selectedi		= 0;
	rept_menu.maxarl		= 0;
	rept_menu.goback		= 0;

	strcpy( rept_menu.MenuId, "REPARTITION" );
	strcpy( rept_menu.backCommand, "goto_sett" );
	
	char command[64];
	for ( unsigned i = 0; i < MAX_NUM_PART; i++ )
	{
		if ( strlen( vparts.pdef[i].name ) == 0 )
			break;

		// Skip variable partition
		if ( vparts.pdef[i].asize )
			continue;

		strcpy( command, "rept_" );
		strcat( command, vparts.pdef[i].name );
		add_menu_item( &rept_menu, vparts.pdef[i].name, command );
	}

	add_menu_item( &rept_menu, "PART STAT", "prnt_stat" );
	add_menu_item( &rept_menu, "COMMIT", "rept_commit" );
	
	active_menu = &main_menu;
	thread_sleep(80);
	redraw_menu();
	start_keylistener();
}

void eval_keydown(void)
{
	if (keyp == ERR_KEY_CHANGED)
		return;

    switch (keys[keyp])
	{
        case KEY_VOLUMEUP:
			menu_item_up();
			break;

        case KEY_VOLUMEDOWN:
			menu_item_down();
			break;

        case KEY_SEND: // dial
            eval_command();
			break;

        case KEY_CLEAR: // hangup
			break;

        case KEY_BACK:
			active_menu->goback = 1;
			eval_command();
			break;
	}
}

void eval_keyup(void)
{
	if (keyp == ERR_KEY_CHANGED)
		return;

    switch (keys[keyp])
	{
        case KEY_VOLUMEUP:
			break;
        case KEY_VOLUMEDOWN:
			break;
        case KEY_SEND: // dial
			break;
        case KEY_CLEAR: //hangup
			break;
        case KEY_BACK:
			break;
	}
}

int key_repeater(void *arg)
{
	uint16_t last_key_i_remember = keyp;
	uint8_t counter1 = 0;
	
	for(;;)
	{
		if ((keyp == ERR_KEY_CHANGED || (last_key_i_remember!=keyp)))
		{
			thread_exit(0);
			return 0;
		} 
		else 
		{
			thread_sleep(10);
			counter1++;
			if(counter1>75)
			{
				counter1=0;
				break;
			}
		}
	}

	while((keyp!=ERR_KEY_CHANGED)&&(last_key_i_remember==keyp)&&(keys_get_state(keys[keyp])!=0))
	{
		eval_keydown();
		thread_sleep(100);
	}

	thread_exit(0);
	return 0;
}
int key_listener(void *arg)
{
	for (;;)
	{
        for(uint16_t i=0; i< sizeof(keys)/sizeof(uint16_t); i++)
		{
			if (keys_get_state(keys[i]) != 0)
			{
				keyp = i;
				eval_keydown();
				thread_resume(thread_create("key_repeater", &key_repeater, NULL, DEFAULT_PRIORITY, 4096));
				while (keys_get_state(keys[keyp]) !=0)
					thread_sleep(1);
				eval_keyup();
				keyp = 99;
			}
		}
	}
	thread_exit(0);
	return 0;
}

void start_keylistener(void)
{
	thread_resume(thread_create("key_listener", &key_listener, 0, LOW_PRIORITY, 4096));
}

void draw_clk_header(void)
{
	fbcon_setfg(0x02E0);
	printf("\n   LEO100 HX-BC SHIP S-OFF\n   HBOOT-%s\n   TOUCH PANEL-MicroP(LED) 0x05\n"
		"   SPL-%s\n   RADIO-%s\n   %s\n\n\n", PSUEDO_VERSION, spl_version, radio_version, BUILD_DATE);
	fbcon_settg(0xffff);fbcon_setfg(0xC3A0);
	_dputs("   <VOL UP> to previous item\n   <VOL DOWN> to next item\n   <CALL> to select item\n   <BACK> to return\n\n\n");
	fbcon_setfg(0x0000);
	if (active_menu != NULL)
	{
		_dputs("   ");
		fbcon_settg(0x001f);fbcon_setfg(0xffff);
		_dputs(active_menu->MenuId);
		_dputs("\n\n\n");
		fbcon_settg(0xffff);fbcon_setfg(0x0000);
	}
}

void cmd_powerdown(const char *arg, void *data, unsigned sz)
{
	printf( "Powering down the device\n");
	fastboot_okay("Device Powering Down");
	shutdown();
	thread_exit(0);
}

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

unsigned target_pause_for_battery_charge(void);
void htcleo_boot(void* kernel,unsigned machtype,void* tags);

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

	if (ramdisk_size)
	{
		*ptr++ = 4;
		*ptr++ = 0x54420005;
		*ptr++ = (unsigned)ramdisk;
		*ptr++ = ramdisk_size;
	}

	ptr = target_atag_mem(ptr);

	if ((ptable = flash_get_ptable()) && (ptable->count != 0))
	{
		int i;
		for(i=0; i < ptable->count; i++)
		{
			struct ptentry *ptn;
			ptn =  ptable_get(ptable, i);
			if (ptn->type == TYPE_APPS_PARTITION)
				pcount++;
		}
		*ptr++ = 2 + (pcount * (sizeof(struct atag_ptbl_entry) / sizeof(unsigned)));
		*ptr++ = 0x4d534d70;
		for (i = 0; i < ptable->count; ++i)
			ptentry_to_tag(&ptr, ptable_get(ptable, i));
	}

	if (cmdline && cmdline[0])
	{
		cmdline_len = strlen(cmdline);
		have_cmdline = 1;
	}
	if (target_pause_for_battery_charge())
	{
		pause_at_bootup = 1;
		cmdline_len += strlen(battchg_pause);
	}

	if (cmdline_len > 0)
	{
		const char *src;
		char *dst;
		unsigned n;
		/* include terminating 0 and round up to a word multiple */
		n = (cmdline_len + 4) & (~3);
		*ptr++ = (n / 4) + 2;
		*ptr++ = 0x54410009;
		dst = (char *)ptr;
		if (have_cmdline)
		{
			src = cmdline;
			while ((*dst++ = *src++));
		}
		if (pause_at_bootup)
		{
			src = battchg_pause;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}
		ptr += (n / 4);
	}

	/* END */
	*ptr++ = 0;
	*ptr++ = 0;

	printf( "booting Linux @ %p, ramdisk @ %p (%d)\n", kernel, ramdisk, ramdisk_size);
	if (cmdline)
		printf( "cmdline: %s\n", cmdline);

	enter_critical_section();
	platform_uninit_timer();
	arch_disable_cache(UCACHE);
	arch_disable_mmu();

#if DISPLAY_SPLASH_SCREEN
	display_shutdown();
#endif

	target_uninit();
	entry(0, machtype, tags);
}

unsigned page_size = 0;
unsigned page_mask = 0;

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

int boot_linux_from_flash(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	unsigned n;
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned offset = 0;
	char *cmdline;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		goto failed;
	}

	/* Fixed Hang after failed boot */
	if (boot_into_recovery)
	{ 
		// Boot from recovery
		boot_into_sboot = 0;
		dprintf(ALWAYS,"\n\nBooting to recovery ...\n\n");
		
		ptn = ptable_find(ptable, "recovery");
		if (ptn == NULL)
		{
			dprintf(CRITICAL, "ERROR: No recovery partition found\n");
			boot_into_recovery=0;
			goto failed;
		}
	}
	else if (boot_into_sboot)
	{ 
		//Boot from sboot partition
		printf("\n\nBooting from sboot partition ...\n\n");
		ptn = ptable_find(ptable,"sboot");
		if (ptn == NULL)
		{
			dprintf(CRITICAL,"ERROR: No sboot partition found!\n");
			boot_into_sboot=0;
			goto failed;
		}
	}
	else 
	{ 
		// Standard boot
		printf("\n\nNormal boot ...\n\n");
		ptn = ptable_find(ptable, "boot");
		if (ptn == NULL)
		{
			dprintf(CRITICAL, "ERROR: No boot partition found\n");
			goto failed;
		}
	}

	if (flash_read(ptn, offset, buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
		goto failed;
	}
	offset += page_size;

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE))
	{
		dprintf(CRITICAL, "ERROR: Invaled boot image heador\n");
		goto failed;
	}

	if (hdr->page_size != page_size)
	{
		dprintf(CRITICAL, "ERROR: Invalid boot image pagesize. Device pagesize: %d, Image pagesize: %d\n",page_size,hdr->page_size);
		goto failed;
	}

	n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
	if (flash_read(ptn, offset, (void *)hdr->kernel_addr, n))
	{
		dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
		goto failed;
	}
	offset += n;

	n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
	if (flash_read(ptn, offset, (void *)hdr->ramdisk_addr, n))
	{
		dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
		goto failed;
	}
	offset += n;

	printf( "kernel  @ %x (%d bytes)\n", hdr->kernel_addr, hdr->kernel_size);
	printf( "ramdisk @ %x (%d bytes)\n", hdr->ramdisk_addr, hdr->ramdisk_size);

	if (hdr->cmdline[0])
		cmdline = (char*) hdr->cmdline;
	else
		cmdline = "";

	strcat(cmdline," clk=1.5.0.0");
	printf( "cmdline = '%s'\n", cmdline);

	printf( "Booting Linux ...\n");
	boot_linux((void *)hdr->kernel_addr, (void *)TAGS_ADDR,
		   (const char *) cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);
	return 0;
failed:
	{
		printf("\n\n   AN IRRECOVERABLE ERROR OCCURED, REBOOTING TO BOOTLOADER.");
		thread_sleep(800);
		reboot_device(FASTBOOT_MODE);
		return -1;
	}
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	static struct boot_img_hdr hdr;
	char *ptr = ((char*) data);

	if (sz < sizeof(hdr))
	{
		fastboot_fail("invalid bootimage header");
		return;
	}

	memcpy(&hdr, data, sizeof(hdr));

	/* ensure commandline is terminated */
	hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

	kernel_actual = ROUND_TO_PAGE(hdr.kernel_size, page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr.ramdisk_size, page_mask);

	memmove((void*) KERNEL_ADDR, ptr + page_size, hdr.kernel_size);
	memmove((void*) RAMDISK_ADDR, ptr + page_size + kernel_actual, hdr.ramdisk_size);

	fastboot_okay("Booting Linux ...");
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
	if (ptable == NULL)
	{
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL)
	{
		fastboot_fail("unknown partition name");
		return;
	}

	if (flash_erase(ptn))
	{
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("Partition Erased successfully");
}

void cmd_flash(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned extra = 0;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) 
	{
		fastboot_fail("unknown partition name");
		return;
	}

	if (!strcmp(ptn->name, "boot") || !strcmp(ptn->name, "recovery") || !strcmp(ptn->name, "sboot")) 
	{
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE))
		{
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(ptn->name, "system") || !strcmp(ptn->name, "userdata") || !strcmp(ptn->name, "persist"))
		extra = ((page_size >> 9) * 16);
	else
		sz = ROUND_TO_PAGE(sz, page_mask);

	printf( "writing %d bytes to '%s'\n", sz, ptn->name);
	if (flash_write(ptn, extra, data, sz))
	{
		fastboot_fail("flash write failure");
		return;
	}

	printf( "partition '%s' updated\n", ptn->name);
	fastboot_okay("Partition Update Successful.");
}

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("Initiating normal Routine.");
	target_battery_charging_enable(0, 1);
	udc_stop();
	boot_linux_from_flash();
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	printf( "rebooting the device\n");
	fastboot_okay("Rebooting Device.");
	reboot_device(0);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	printf( "rebooting the device\n");
	fastboot_okay("Rebooting Device to HBOOT.");
	reboot_device(FASTBOOT_MODE);
}

void send_mem(char* start, int len)
{
	char response[64];
	while(len>0)
	{
		int slen = len > 29 ? 29 : len;
		memcpy(response, "INFO", 4);
		response[4]=slen;

		for(int i=0; i<slen; i++)
		{
			response[5+i*2] = charVal(start[i]>>4);
			response[5+i*2+1]= charVal(start[i]);
		}
		response[5+slen*2+1]=0;
		fastboot_write(response, 5+slen*2);

		start+=slen;
		len-=slen;
	}
}

void cmd_oem_smesg()
{
	send_mem((char*)0x1fe00018, MIN(0x200000, readl(0x1fe00000)));
	fastboot_okay("");
}

void cmd_oem_dmesg()
{
	if(*((unsigned*)0x2FFC0000) == 0x43474244 /* DBGC */  ) //see ram_console_buffer in kernel ram_console.c
	{
		send_mem((char*)0x2FFC000C, *((unsigned*)0x2FFC0008));
	}
	fastboot_okay("");
}
void cmd_oem_dumpmem(const char *arg)
{
	char *sStart = strtok((char*)arg, " ");
	char *sLen = strtok(NULL, " ");
	if(sStart==NULL || sLen==NULL)
	{
		fastboot_fail("usage:oem dump start len");
		return;
	}

	send_mem((char*)str2u(sStart), str2u(sLen));
	fastboot_okay("");
}

void cmd_oem_set(const char *arg)
{
	char type=*arg; arg++;
	char *sAddr = strtok((char*) arg, " ");
	char *sVal = strtok(NULL, "\0");
	if(sAddr==NULL || sVal==NULL)
	{
		fastboot_fail("usage:oem set[s,c,w] address value");
		return;
	}
	char buff[64];
	switch(type)
	{
		case 's':
			memcpy((void*)str2u(sAddr), sVal, strlen(sVal));
			send_mem((char*)str2u(sAddr), strlen(sVal));
			break;
		case 'c':
			*((char*)str2u(sAddr)) = (char)str2u(sVal);
			sprintf(buff, "%x", *((char*)str2u(sAddr)));
			send_mem(buff, strlen(buff));
			break;
		case 'w':
		default:
			*((int*)str2u(sAddr)) = str2u(sVal);
			sprintf(buff, "%x", *((int*)str2u(sAddr)));
			send_mem(buff, strlen(buff));
	}
	fastboot_okay("");
}

void cmd_oem_cls()
{
	redraw_menu();
	fastboot_okay("");
}

void cmd_oem_part_add(const char *arg)
{
	vpart_add(arg);
	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_resize(const char *arg)
{
	vpart_resize(arg);
	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_del(const char *arg)
{
	vpart_del(arg);
	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_commit()
{
	vpart_commit();
	printf("\n Partition changes saved! Will Reboot device in 2 secs\n");
	fastboot_okay("");
	thread_sleep(1980);
	reboot_device(FASTBOOT_MODE);
}

void cmd_oem_part_read()
{
	vpart_read();
	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_list()
{
	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_clear()
{
	vpart_clear();

	vpart_list();
	fastboot_okay("");
}

void cmd_oem_part_create_default()
{
	vpart_clear();
	vpart_create_default();

	vpart_list();
	fastboot_okay("");
}

void prnt_nand_stat(void)
{
	struct flash_info *flash_info;
	struct ptable *ptable;
	struct ptentry* ptn;

	flash_info = flash_get_info();
	if ( flash_info == NULL )
	{
		dprintf( CRITICAL, "ERROR: flash info unavailable!!!\n" );
		return;
	}

	ptable = flash_get_vptable();
	if ( ptable == NULL )
	{
		dprintf( CRITICAL, "ERROR: VPTABLE table not found!!!\n" );
		return;
	}

	ptn = ptable_find( ptable, PTN_VPTABLE );
	if ( ptn == NULL )
	{
		dprintf( CRITICAL, "ERROR: VPTABLE partition not found!!!\n" );
		return;
	}
	
	printf("\n========================= NAND INFO ========================\n\n");
	
	printf("  Flash block size: %i bytes\n", flash_info->block_size );
	printf("  Flash page size: %i bytes\n", flash_info->page_size );
	printf("  Flash spare size: %i\n", flash_info->spare_size );
	printf("  Flash total size: %i blocks - %i MB\n", flash_info->num_blocks, (int)( flash_info->num_blocks / get_blk_per_mb() ) );
	printf("  ROM (0x400) offset: 0x%x\n", HTCLEO_ROM_OFFSET );
	printf("  VPTABLE offset: 0x%x size: %i blocks\n", ptn->start, ptn->length );
	printf("  PTABLE offset: 0x%x size: %i blocks - %i MB\n", get_flash_offset(), get_full_flash_size(), (int)( get_full_flash_size() / get_blk_per_mb() ) );
	printf("  Usable flash size: %i blocks - %i MB\n", get_usable_flash_size(), (int)( get_usable_flash_size() / get_blk_per_mb() ) );
	printf("  ExtROM offset: 0x%x size: %i blocks - %i MB\n", get_ext_rom_offset(), get_ext_rom_size(), (int)( get_ext_rom_size() / get_blk_per_mb() ) );
	printf("\n========================= NAND INFO ========================\n");
}

void cmd_oem_nand_status(void)
{
	prnt_nand_stat();
	fastboot_okay("");
}

void cmd_oem_part_format_all()
{
	
	struct ptentry *ptn;
	struct ptable *ptable;
	
	printf("\n\n\nInitializing flash format...\n");
	
	ptable = flash_get_vptable();
	if (ptable == NULL) 
	{
		printf( "ERROR: VPTABLE not found!!!\n");
		return;
	}

	ptn = ptable_find(ptable, "task29");
	if (ptn == NULL) 
	{
		printf( "ERROR: No vptable partition!!!\n");
		return;
	}
	printf("Formating flash...\n");
	printf("\n============================================================\n\n");
	
	flash_erase(ptn);
	printf("\n============================================================\n\n");
	
	printf("\nFormat complete ! \nReboot device to create default partition table, or create partitions manualy!\n\n");
	
	fastboot_okay("");
}

void cmd_oem_part_format_vpart()
{	
	struct ptentry *ptn;
	struct ptable *ptable;
	
	printf("\n\n\nInitializing flash format...\n");
	
	ptable = flash_get_vptable();
	if (ptable == NULL)
	{
		printf( "ERROR: VPTABLE not found!!!\n");
		return;
	}

	ptn = ptable_find(ptable, "vptable");
	if (ptn == NULL)
	{
		printf( "ERROR: No vptable partition!!!\n");
		return;
	}
	
	printf("Formating vptable...\n");
	
	printf("\n============================================================\n\n");
	
	flash_erase(ptn);
	
	printf("\n============================================================\n\n");

	printf("\nFormat complete ! \nReboot device to create default partition table, or create partitions manually!\n\n");

	fastboot_okay("");

	return;
}

void cmd_test()
{
	int i =0;
	for (i=0;i<100;i++)
		printf("Test line... %i\n", i);
	fastboot_okay("Hello!");
	redraw_menu();
}

void cmd_oem_help()
{
	fbcon_resetdisp();

	printf("\n======================= FASTBOOT HELP =======================\n");

	printf(" fastboot oem help");
	printf("  - This simple help\n\n");
	
	printf(" fastboot oem cls");
	printf("  - Clear screen\n\n");
	
	printf(" fastboot oem part-add name:size\n");
	printf("  - Create new partition with given name and size (in Mb, 0 for all space)\n");
	printf("  - example: fastboot oem part-add system:160  - this will create system partition with size 160Mb\n\n");
	
	printf(" fastboot oem part-resize name:size\n");
	printf("  - Resize partition with given name to new size (in Mb, 0 for all space)\n");
	printf("  - example: fastboot oem part-resize system:150  - this will resize system partition to 150Mb\n\n");
	
	printf(" fastboot oem part-del name");
	printf("  - Delete named partition\n\n");
	
	printf(" fastboot oem part-create-default\n");
	printf("  - Delete curent partition table and create default one\n\n");
	
	printf(" fastboot oem part-read");
	printf("  - Reload partition layout from NAND\n\n");
	
	printf(" fastboot oem part-commit\n");
	printf("  - Save curent layout to NAND. All changes to partition layout are tmp. until committed to nand!\n\n");
	
	printf(" fastboot oem part-list");
	printf("  - Display current partition layout\n\n");
	
	printf(" fastboot oem part-clear");
	printf("  - Clear current partition layout\n\n");
	
	printf(" fastboot oem format-all\n");
	printf("  - WARNING !!!  THIS COMMAND WILL FORMAT COMPLETE NAND (except bootloader)\n   - ALSO IT WILL DISPLAY BAD SECTORS IF THERE ARE ANY\n");
	printf("  - !!!  THIS IS EQUIVALENT TO TASK 29 !!!\n\n");
	
	fastboot_okay("Look at your Device.");
}

static int flashlight(void *arg)
{
	if ( target_support_flashlight() )
	{
		for(;;)
		{
			if(keys_get_state_n(0x123)!=0)break;
			udelay(   (unsigned)(((unsigned)target_flashlight())-2)   );
		}
	}
	thread_exit(0);
	return 0;
}

void cmd_flashlight(void)
{
	thread_resume((thread_t *)thread_create("Flashlight", &flashlight, NULL, HIGHEST_PRIORITY, DEFAULT_STACK_SIZE));
	return;
}

void cmd_oem(const char *arg, void *data, unsigned sz)
{
	while(*arg==' ') arg++;
	if(memcmp(arg, "cls", 3)==0)                           cmd_oem_cls();
	if(memcmp(arg, "set", 3)==0)                           cmd_oem_set(arg+3);
	if(memcmp(arg, "set", 3)==0)                           cmd_oem_set(arg+3);
	if(memcmp(arg, "pwf ", 4)==0)                          cmd_oem_dumpmem(arg+4);
	if(memcmp(arg, "test", 4)==0)                          cmd_test();
	if(memcmp(arg, "dmesg", 5)==0)                         cmd_oem_dmesg();
	if(memcmp(arg, "smesg", 5)==0)                         cmd_oem_smesg();
	if(memcmp(arg, "nandstat", 8)==0)                      cmd_oem_nand_status();
	if(memcmp(arg, "poweroff", 8)==0)                      cmd_powerdown(arg+8, data, sz);
	if(memcmp(arg, "part-add ", 9)==0)                     cmd_oem_part_add(arg+9);
	if(memcmp(arg, "part-del ", 9)==0)                     cmd_oem_part_del(arg+9);
	if(memcmp(arg, "part-read", 9)==0)                     cmd_oem_part_read();
	if(memcmp(arg, "part-list", 9)==0)                     cmd_oem_part_list();
	if(memcmp(arg, "part-clear", 10)==0)                   cmd_oem_part_clear();
	if(memcmp(arg, "format-all", 10)==0)                   cmd_oem_part_format_all();
	if(memcmp(arg, "part-commit", 11)==0)                  cmd_oem_part_commit();
	if(memcmp(arg, "part-resize ", 12)==0)                 cmd_oem_part_resize(arg+12);
	if(memcmp(arg, "format-vpart", 12)==0)                 cmd_oem_part_format_vpart();
	if(memcmp(arg, "part-create-default", 19)==0)          cmd_oem_part_create_default();
	if((memcmp(arg,"help",4)==0)||(memcmp(arg,"?",1)==0))  cmd_oem_help();
}

void target_init_fboot(void)
{
	// Initiate Fastboot.
	udc_init(&surf_udc_device);
	fastboot_register("oem", cmd_oem);
	fastboot_register("boot", cmd_boot);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("continue", cmd_continue);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bootloader);
	fastboot_register("powerdown", cmd_powerdown);
	fastboot_publish("version", "1.0");
	fastboot_publish("version-bootloader", "1.0");
	fastboot_publish("version-baseband", "unset");
	fastboot_publish("serialno", "HTC HD2");
	fastboot_publish("secure", "no");
	fastboot_publish("product", TARGET(BOARD));
	fastboot_publish("kernel", "lk");
	fastboot_publish("author", "Shantanu Gupta, Danijel Posilovic, Arif Ali, Cedesmith, QuiC, Travis Geiselbrecht.");
	fastboot_init(target_get_scratch_address(),MEMBASE - SCRATCH_ADDR - 0x00100000);
	udc_start();
	target_battery_charging_enable(1, 0);
}

char ccharVal(char c)
{
	c&=0xf;
	if(c<=9) return '0'+c;
	return 'A'+(c-10);
}

void dump_mem(char* start, int len)
{
	char response[64];
	while(len>0)
	{
		int slen = len > 29 ? 29 : len;
		memcpy(response, "INF ", 4);
		response[4]=slen;

		for(int i=0; i<slen; i++)
		{
			response[5+i*2] = ccharVal(start[i]>>4);
			response[5+i*2+1]= ccharVal(start[i]);
		}
		response[5+slen*2+1]=0;
		_dputs(response);
		start+=slen;
		len-=slen;
	}
	_dputs("\n");
}
static int update_header_str(void *arg)
{
	while(!(strlen(spl_version))){update_spl_ver();}
	char expected_byte[] = "3";
	while(radBuffer[0] != expected_byte[0]){update_radio_ver();}

	redraw_menu();

	thread_exit(0);
	return 0;
}
void aboot_init(const struct app_descriptor *app)
{
	page_size = flash_page_size();
	page_mask = page_size - 1;

	/* Check if we should do something other than booting up */
	if (keys_get_state(keys[6]) != 0)
		boot_into_recovery = 1;
	if (keys_get_state(keys[5]) != 0)
		goto bmenu;
	if (keys_get_state(keys[2]) != 0)
		display_init();

 	if (check_reboot_mode() == RECOVERY_MODE)
 		boot_into_recovery = 1;
 	else if(check_reboot_mode() == FASTBOOT_MODE) 
		goto bmenu;

	if (boot_into_recovery == 1)
		recovery_init();

	/* Environment setup for continuing, Lets boot if we can */
 	boot_linux_from_flash();

	/* Couldn't Find anything to do (OR) User pressed Back Key. Load Menu */
bmenu:
	thread_resume(thread_create("hdrs", &update_header_str, NULL, LOW_PRIORITY, DEFAULT_STACK_SIZE));
	display_init();
	target_init_fboot();
	init_menu();
}

APP_START(aboot)
	.init = aboot_init,
APP_END
