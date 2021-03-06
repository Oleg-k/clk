/*
 * Copyright (c) 2008, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2010, Code Aurora Forum. 
 * All rights reserved.
 *
 * Copyright (c) 2011, Shantanu/zeusk. 
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <debug.h>
#include <err.h>
#include <stdlib.h>
#include <dev/fbcon.h>

#include "font8x16.h"

#define RGB565_BLACK		0x0000
#define RGB565_WHITE		0xffff

#define RGB888_BLACK            0x000000
#define RGB888_WHITE            0xffffff

struct pos
{
	int x;
	int y;
};

static struct fbcon_config *config = NULL;

uint16_t *pixor;

static uint16_t			BGCOLOR;
static uint16_t			FGCOLOR;
static uint16_t			TGCOLOR;

static struct pos		cur_pos;
static struct pos		max_pos;

static bool			scrolled;
static bool			forcedtg;

static void ijustscrolled(void)
{
	scrolled = true;
}

static void cleanedyourcrap(void)
{
	scrolled = false;
}

bool didyouscroll(void)
{
	return scrolled;
}

void fbcon_forcetg(bool flag_boolean)
{
	forcedtg = flag_boolean;
}

int fbcon_get_x(void)
{
    return cur_pos.x;
}
int fbcon_get_y(void)
{
    return cur_pos.y;
}
void fbcon_set_x(int offset)
{
    cur_pos.x = offset;
}
void fbcon_set_y(int offset)
{
    cur_pos.y = offset;
}
static unsigned reverse_fnt_byte(unsigned x)
{
	unsigned y = 0;
	for (uint8_t i = 0; i < 9; ++i)
	{
		y <<= 1;
		y |= (x & 1);
		x >>= 1;
	}
	return y;
}
static void fbcon_drawglyph_helper(uint16_t *pixels, unsigned stride, unsigned data, bool dtg)
{
	for (unsigned y = 0; y < (FONT_HEIGHT / FONT_PPCHAR); y++)
	{
		data = reverse_fnt_byte(data);
		for (unsigned x = 0; x < FONT_WIDTH; x++)
		{
			if (data & 1) *pixor = FGCOLOR;
			else if(dtg) *pixor = TGCOLOR;
			data >>= 1;
			pixor++;
		}
		pixor += stride;
	}
	return;
}
static void fbcon_drawglyph(uint16_t *pixels, unsigned stride, unsigned *glyph)
{
	stride -= FONT_WIDTH;
	bool dtg = false;	

	if ((BGCOLOR != TGCOLOR) || (forcedtg)) {dtg = true;}

	pixor = pixels;
	for(unsigned i = 0; i < FONT_PPCHAR; i++)
	{
		fbcon_drawglyph_helper(pixor, stride, glyph[i], dtg);
	}
	return;
}
#if LCD_REQUIRE_FLUSH
static void fbcon_flush(void)
{
	if (config->update_start)
		config->update_start();
	if (config->update_done)
		while (!config->update_done());
}
#endif
static void fbcon_scroll_up(void)
{
	unsigned buffer_size = (config->width * config->height) * (config->bpp /8);
	unsigned line_size = (config->width * FONT_HEIGHT) * (config->bpp / 8);
	memmove(config->base, config->base + line_size, buffer_size-line_size);
	memset(config->base+buffer_size-line_size,BGCOLOR,line_size);
	ijustscrolled();
#if LCD_REQUIRE_FLUSH
	fbcon_flush();
#endif
}
void fbcon_fill_rect ( unsigned ix, unsigned iy, unsigned cpy_h, unsigned cpy_w, uint16_t paint )
{
	if ((iy + cpy_h) > config->height) return;
	if ((ix + cpy_w) > config->width) return;

	unsigned line_size = config->width * (config->bpp / 8);
	unsigned cpy_size = cpy_w * (config->bpp / 8);

	for (unsigned i = 0; i < (cpy_h - iy); i++)
	{
		memset ( config->base + ( (iy*line_size) + (i*line_size) + (line_size-cpy_size) ), paint, cpy_size);
	}
}
static void fbcon_clear(void)
{
	unsigned count = config->width * config->height;
	memset(config->base, BGCOLOR, count * ((config->bpp) / 8));
	cleanedyourcrap();
}
void fbcon_reset(void)
{
	fbcon_clear();
	cur_pos.x = 0;
	cur_pos.y = 0;
	cleanedyourcrap();
}

void fbcon_set_colors(bool sbg, bool sfg, bool stg, unsigned bg, unsigned fg, unsigned tg)
{
	if (sbg) BGCOLOR = bg;
	if (sfg) FGCOLOR = fg;
	if (stg) TGCOLOR = tg;
}

void fbcon_putc(char c)
{
	uint16_t *pixels;

	/* ignore anything that happens before fbcon is initialized */
	if (!config)
		return;

	if((unsigned char)c > 127)
		return;
	if((unsigned char)c < 32) {
		if(c == '\n')
			goto newline;
		else if (c == '\r')
			cur_pos.x = 0;
		return;
	}

	pixels = config->base;
	pixels += cur_pos.y * FONT_HEIGHT * config->width;
	pixels += cur_pos.x * (FONT_WIDTH);
	fbcon_drawglyph(pixels, config->stride, font8x16 + (c - 32) * FONT_PPCHAR);

	cur_pos.x++;
	if (cur_pos.x < max_pos.x)
		return;

newline:
	cur_pos.y++;
	cur_pos.x = 0;
	if(cur_pos.y >= max_pos.y) {
		cur_pos.y = max_pos.y - 1;
		fbcon_scroll_up();
	}
#if LCD_REQUIRE_FLUSH
	else fbcon_flush();
#endif
}
void fbcon_init_colors(void)
{
	uint32_t bg;
	uint32_t fg;

	switch (config->format) {
	case FB_FORMAT_RGB565:
		fg = RGB565_WHITE;
		bg = RGB565_BLACK;
		break;
        case FB_FORMAT_RGB888:
                fg = RGB888_WHITE;
                bg = RGB888_BLACK;
                break;
	default:
		dprintf(CRITICAL, "unknown framebuffer pixel format\n");
		ASSERT(0);
		break;
	}

	fbcon_set_colors(1,1,1,bg, fg, bg); //Background, Foreground, (Text/Pen)Ground
}
void fbcon_setup(struct fbcon_config *_config)
{
	ASSERT(_config);

	config = _config;

	fbcon_init_colors();

	cur_pos.x = 0;
	cur_pos.y = 0;
	max_pos.x = config->width / (FONT_WIDTH);
	max_pos.y = (config->height - 1) / FONT_HEIGHT;
}

struct fbcon_config* fbcon_display(void)
{
    return config;
}

static inline unsigned long pixel_to_pat( uint32_t bpp, uint32_t pixel) /* cfb & msm fb driver of linux */
{
	switch (config->bpp) {
	case 1:
		return 0xfffffffful*pixel;
	case 2:
		return 0x55555555ul*pixel;
	case 4:
		return 0x11111111ul*pixel;
	case 8:
		return 0x01010101ul*pixel;
	case 12:
		return 0x01001001ul*pixel;
	case 16:
		return 0x00010001ul*pixel;
	case 24:
		return 0x01000001ul*pixel;
	case 32:
		return 0x00000001ul*pixel;
	default:
		return 0x000000000;
    }
}