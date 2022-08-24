/*
 * cga16dmo.cpp
 *
 *  Created on: Dec 19, 2019
 *      Author: Perry Harrington
 *		License: MIT (do whatever you want with it)
 *
 *	Modified to build in DJGPP by Vojtěch Salajka, Aug 24, 2022:
 *		i586-pc-msdosdjgpp-g++ -O2 CGA16DMO.CPP -s -o CGA16DMO.EXE
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

#define CRTCa 0x3d8	/* 6845 mode control register */
#define CRTCb 0x3d4 /* 6845 index register */
#define SR	0x3da	/* Input Status Register */
#define TEXT 0x03   /* DOS video mode for 80x25 text */

static unsigned char screen[16000];
static unsigned char *buffer;

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

/* wait for vsync period to synchronize actions */
void vsync(void)
{
    unsigned char p;

	do {
		p=inportb(SR);
	} while (p & 8);

	do {
		p=inportb(SR);
	} while (!(p & 8));
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

	dpmiRegs.h.ah=0x1a;
	dpmiRegs.h.al=0;
	__dpmi_int(0x10, &dpmiRegs);		/* get current display settings from BIOS (VGA only) */

	active = dpmiRegs.h.bl;
	status = dpmiRegs.h.al;

	if (status == 0x1a && (active == 0x07 || active == 0x08)) {  /* VGA color or mono*/

		/* Read the input status register to reset the VGA attribute controller index/data state */
		status=inportb(SR);

		/* VGA attribute controller index register, mode register select */
		outportb(0x3c0,0x10);

		/* VGA attribute controller data register, read mode register */
		status=inportb(0x3c1);

		status&=0xf7;	/* turn off bit 3, blink */

		outportb(0x3c0,0x10);	/* select the attribute control register */

		outportb(0x3c0,status);	/* write to VGA attribute controller data register */
			
		/* VGA has an 8x16 character cell and 4 lines makes a square
			since VGA has a 1:1 pixel aspect ratio */
		write_crtc(CRTCb,0x09,0x03);

		return 3;	/* VGA */

	} else if ( /* EGA */
				switches == 0x6 ||	/* CGA w/CGA 40x25 */
				switches == 0x7 ||	/* CGA w/CGA 80x25 */
				switches == 0x8 ||	/* EGA w/CGA 80x25 */
				switches == 0x9 ||	/* EGA w/ECD 80x25 */
				switches == 0xB 	/* EGA w/MONO */
				) {

		dpmiRegs.h.al=0x03;
		dpmiRegs.h.ah=0x10;
		dpmiRegs.h.bl=0;
		__dpmi_int(0x10, &dpmiRegs);		/* turn off blink via EGA BIOS */

		/* EGA hires mode is 640x350 with a 9x14 character cell.  The pixel aspect
			ratio is 1:1.37, so if we make the blocks 3 scans tall you get a square
			pixel at 160x100, but some of the scan lines are not used (50) */
		
		if (
			switches == 0x09 ||		/* EGA Hires monitor attached, 9x14 */
			switches == 0xB		/* EGA with Monochrome monitor, 9x14 */
			) {
				write_crtc(CRTCb,0x09,0x02);
			} else {						/* Must be CGA 8x8 on EGA card */
				write_crtc(CRTCb,0x09,0x01);
			}

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

			buffer = (unsigned char  *)0xb8000 + __djgpp_conventional_base;

			for (int i=0;i<16000;i+=2) {
				/* Set the video memory to ▐ */
				screen[i]=0xde;
				/* assign black color */
				screen[i+1]=0;
			}
			vsync();
			memcpy(buffer,screen,16000);

			return 1;	/* CGA */

		}
	}

	return 0;
}

int main(void)
{
	if ( !(_crt0_startup_flags & _CRT0_FLAG_NEARPTR) )
	if (!__djgpp_nearptr_enable ())
		return 0;

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
			
			screen[i+16000-320] = k;
			screen[i+16000-160] = k;
		}
		vsync();
		{
			unsigned char* buffout = buffer + 1;
			unsigned char* scrin = screen;
			union { struct { unsigned char a : 4; unsigned char b : 4; } p; unsigned char ab; } pack;
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
		++frame;
	}
	text();
	return 0;
}
