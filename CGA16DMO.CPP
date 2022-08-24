/*
 * cga16dmo.cpp
 *
 *  Created on: Dec 19, 2019
 *      Author: Perry Harrington
 *		License: MIT (do whatever you want with it)
 *
 *	Modified to build in DJGPP by Vojtěch Salajka, Aug 24, 2022:
 *		i586-pc-msdosdjgpp-g++ -O2 -funroll-loops -fexpensive-optimizations CGA16DMO.CPP -s -o CGA16DMO.EXE
 */

#include <dos.h>
#include <conio.h>
#include <stdlib.h>
#include <stdio.h>
#include <mem.h>
#include <dpmi.h>
#include <sys/nearptr.h>
#include <crt0.h>
#include <string.h>

#define CRTCa			0x3d8	/* 6845 mode control register */
#define CRTCb			0x3d4	/* 6845 index register */
#define SR				0x3da	/* Input Status Register */
#define TEXT			0x03	/* DOS video mode for 80x25 text */
#define HIGH_ADDRESS	0x0C	/* CRT controller registers */
#define LOW_ADDRESS		0x0D
#define DISPLAY_ENABLE	0x01	/* VGA input status bits */
#define VRETRACE		0x08

static unsigned char screen[16000];
static unsigned char *cgabuf; /* CGA page pointer */
static unsigned short visual_page = 0; /* EGA page offsets */
static unsigned short active_page = 320*200/8;
void (*flip)(void);

/* wait for vsync period to synchronize actions */
void vsync_cga(void)
{
	while ((inportb(SR) & VRETRACE));
	while (!(inportb(SR) & VRETRACE));
}

void flip_cga(void)
{
	unsigned char* buffout = cgabuf + 1;
	unsigned char* scrin = screen;
	union { struct { unsigned char a : 4; unsigned char b : 4; } p; unsigned char ab; } pack;
	vsync_cga();
	for (int i = 8000; i != 0; --i)
	{
		pack.p.b = *scrin;
		++scrin;
		pack.p.a = *scrin;
		++scrin;
		*buffout = pack.ab;
		buffout += 2;
	}
}

void ega_page_flip(unsigned short *page1, unsigned short *page2)
{
	unsigned short high_address,low_address;
	unsigned short temp;

	temp=*page1;
	*page1=*page2;
	*page2=temp;

	high_address = HIGH_ADDRESS | (*page1 & 0xff00);
	low_address  = LOW_ADDRESS  | (*page1 << 8);

	while ((inportb(SR) & DISPLAY_ENABLE));
	outpw(CRTCb, high_address);
	outpw(CRTCb, low_address);
	while (!(inportb(SR) & VRETRACE));
}

static unsigned char linebuf[40];

void flip_ega(void)
{
	unsigned char* EGA = (unsigned char*)0xA0000 + active_page + __djgpp_conventional_base;

	for (int p = 0; p != 4; ++p) /*cycle through 4 bitplanes*/
	{
		outportb(0x3C4, 2);
		outportb(0x3C5, 1 << p);
		unsigned char* egap = EGA;
		unsigned char* scrp = screen;
		for (int y = 100; y != 0; --y)
		{
			unsigned char* linep = linebuf;
			for (int x = 40; x != 0; --x)
			{
				unsigned char planeBits = (((*scrp >> p) & 1) << 6);
				++scrp;
				planeBits |= (((*scrp >> p) & 1) << 4);
				++scrp;
				planeBits |= (((*scrp >> p) & 1) << 2);
				++scrp;
				planeBits |= (((*scrp >> p) & 1));
				++scrp;
				planeBits = planeBits | (planeBits << 1);
				*linep = planeBits;
				++linep;
			}
			memcpy(egap, linebuf, 40);
			egap += 40;
			memcpy(egap, linebuf, 40);
			egap += 40;
		}
	}
	ega_page_flip(&visual_page,&active_page);
}

/*
Simple helper routing to write to the index register, then the data register
The 6845 index and data registers are paired, the index register is the base
port address and the data register is one address higher, when writing data
to the 6845 you write the address of the register you want to change to the
index register, then you write the new data to the data register
*/
void write_crtc(unsigned int port, unsigned char reg, unsigned char val)
{
	outportb(port, reg);
	outportb(port+1,val);
}

/*
simple helper to put video card back in standard 80x25 text mode
*/
void text(void)
{
	__dpmi_regs dpmiRegs;
	dpmiRegs.h.al=(unsigned char) TEXT;
	dpmiRegs.h.ah=0;
	__dpmi_int(0x10, &dpmiRegs);
}

/*
This routine detects CGA, EGA, and VGA cards, it sets the attributes
appropriately to enable 160x100 16 color text mode
*/
int x16mode(void)
{
	unsigned char status;
	unsigned char active;
	unsigned char switches;

	text();					/* request 80x25 color mode */

	__dpmi_regs dpmiRegs;

	dpmiRegs.h.ah=0x12;
	dpmiRegs.h.al=0;
	dpmiRegs.h.bl=0x10;
	__dpmi_int(0x10, &dpmiRegs);		/* get EGA switch settings */

	switches = dpmiRegs.h.cl;

	if ( /* EGA */
				switches == 0x6 ||	/* CGA w/CGA 40x25 */
				switches == 0x7 ||	/* CGA w/CGA 80x25 */
				switches == 0x8 ||	/* EGA w/CGA 80x25 */
				switches == 0x9 ||	/* EGA w/ECD 80x25 */
				switches == 0xB 	/* EGA w/MONO */
				) {

		dpmiRegs.h.ah = 0x00;  /* function 00h = mode set */
		dpmiRegs.h.al = 0x0d;  /* EGA 320x200 16 colors */
		__dpmi_int(0x10, &dpmiRegs);

		flip = flip_ega;

		memset(screen, 0, 16000);

		return 2;	/* EGA */
	} else { /* CGA does not have an attribute controller register, only mode controller */

		dpmiRegs.h.ah=0xF;
		dpmiRegs.h.al=0;
		__dpmi_int(0x10, &dpmiRegs);		/* get current display settings from BIOS */

		if (dpmiRegs.h.al != 0x07) {		/* Check that this is not BW 80x25 */

			/* set mode control register for 80x25 text mode and disable video output */
			outportb(CRTCa, 1);

			/*
				These settings put the 6845 into "graphics" mode without actually
				switching the CGA controller into graphics mode.  The register
				values are directly copied from CGA graphics mode register
				settings.  The 6845 does not directly display graphics, the
				6845 only generates addresses and sync signals, the CGA
				attribute controller either displays character ROM data or color
				pixel data, this is external to the 6845 and keeps the CGA card
				in text mode.
				ref: HELPPC
			*/

			/* set vert total lines to 127 */
			write_crtc(CRTCb,0x04,0x7f);
			/* set vert displayed char rows to 100 */
			write_crtc(CRTCb,0x06,0x64);
			/* set vert sync position to 112 */
			write_crtc(CRTCb,0x07,0x70);
			/* set char scan line count to 1 */
			write_crtc(CRTCb,0x09,0x01);

			/* re-enable the video output in 80x25 text mode */
			outportb(CRTCa, 9);

			cgabuf = (unsigned char  *)0xb8000 + __djgpp_conventional_base;

			for (int i=0;i<16000;i+=2) {
				/* Set the video memory to ▐ */
				screen[i]=0xde;
				/* assign black color */
				screen[i+1]=0;
			}
			vsync_cga();
			memcpy(cgabuf,screen,16000);
			memset(screen, 0, 16000);

			flip = flip_cga;

			return 1;	/* CGA */

		}
	}

	return 0;
}

int main(void)
{
	if ( !(_crt0_startup_flags & _CRT0_FLAG_NEARPTR) )
	{
		if (!__djgpp_nearptr_enable ())
		{
			return 0;
		}
	}

	/* enable 160x100 16 color mode */
	int mode=x16mode();
	if (mode==0) {
		printf("No compatible display detected, MDA?\n");
		return 1;
	}

	for (int i=0;i<16000;++i) {
		screen[i]=rand() & 0xf;
	}

	int frame = 0;

	while(!kbhit()) {
		/* scrolling strips */
		for (int i = 0; i != 160; ++i)
		{
			int j = (i + frame) & 0xf;
			int k = (i - frame) & 0xf;
			screen[i] = j;
			screen[i+160] = j;
			screen[i+15680] = k;
			screen[i+15840] = k;
		}
		flip();
		if (mode == 1)
		{
			++frame;
		}
		else
		{
			--frame; /* invert scrolling direction in EGA mode for testing */
		}
	}
	text();
	return 0;
}
