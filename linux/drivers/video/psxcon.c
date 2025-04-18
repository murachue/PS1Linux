/*
 *	Virtual Console over PlayStation GPU.
 * FIXME: Virtual screen resolution is not supported now !!!
 */

#undef FBCONDEBUG

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/delay.h>	/* MSch: for IRQ probe */
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/console_struct.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/malloc.h>
#include <linux/fb.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/smp.h>
#include <linux/init.h>

#include <asm/ps/libpsx.h>
#include <asm/io.h>

#define PSXVGA_SCR_H	21
#define PSXVGA_SCR_W	37

#ifdef CONFIG_VT_CONSOLE_HIRES
#define PSXVGA_SCR_H	21
#define PSXVGA_SCR_W	78 
#endif

#define PSXVGA_VSCR_H	(PSXVGA_SCR_H)
#define PSXVGA_VSCR_W	(PSXVGA_SCR_W)
#define PSXVGA_FNT_H	12
#define PSXVGA_FNT_W	 8


static unsigned int psxvga_scrbuf[PSXVGA_VSCR_H][PSXVGA_VSCR_W];  //PSX TEXT SCREEN BUFFER
static int psxvga_cury;				// POINTER TO CURRENT STRING
static int psxvga_curx;				// POINTER TO CURRENT POSITION IN STR
static int psxvga_bottom;


/*
 *  Interface used by the world
 */

static const char *psxvga_startup(void);
static void psxvga_init(struct vc_data *conp, int init);
static void psxvga_deinit(struct vc_data *conp);
static void psxvga_clear(struct vc_data *conp, int sy, int sx, int height,
		       int width);
static void psxvga_putc(struct vc_data *conp, int c, int ypos, int xpos);
static void psxvga_putcs(struct vc_data *conp, const unsigned short *s, int count,
			int ypos, int xpos);
static void psxvga_cursor(struct vc_data *conp, int mode);
static int  psxvga_scroll(struct vc_data *conp, int t, int b, int dir,
			 int count);
static void psxvga_bmove(struct vc_data *conp, int sy, int sx, int dy, int dx,
			int height, int width);
static int  psxvga_switch(struct vc_data *conp);
static int  psxvga_blank(struct vc_data *conp, int blank);
static int  psxvga_font_op(struct vc_data *conp, struct console_font_op *op);
static int  psxvga_set_palette(struct vc_data *conp, unsigned char *table);
static int  psxvga_scrolldelta(struct vc_data *conp, int lines);


/*
 *  Low Level Operations
 */


static const char *psxvga_startup (void)
{
   const char *display_desc = "PSXGPU console";
   int  x, y;
   unsigned long mode, hrange, vrange;
   for (y = 0; y < PSXVGA_VSCR_H; y++)
      for (x = 0; x < PSXVGA_VSCR_W; x++)
         psxvga_scrbuf[y][x] = 0;
         
   psxvga_cury = 0;	
   psxvga_curx = 0;
   
   // super-hacky NTSC/PAL detection: use ROM version string
   // we can take just 1F801814.20 for normal booting,
   // but for instance PCSX-redux "Load binary" that register does not set correctly.
   // we guess(!) it from ROM area!!
   // note: SCPH-1000 is known to not have the version string.
   // note: SCPH-3000 looks like it have irregular. (not "J" but 199"5")
   // note: alternative ROMs will not have it at all. (NO$PSX have " "(space)? Openbios lacks?)
   if (*(volatile char *)0xBFC7ff52 == 'E') {
      // PAL
#ifdef CONFIG_VT_CONSOLE_HIRES
      mode=0x0800000b; // 640w, 240p, PAL, 15bpp
      //hrange = 0x06cb02b0; // 688 | ((688+2560)<<12) | 0x06000000
      hrange = 0x06c60260; // 608:(608+320*8)
#else
      mode=0x08000009; // 320w, 240p, PAL, 15bpp
      //hrange = 0x06cb02b0; // 688 | ((688+2560)<<12) | 0x06000000
      hrange = 0x06c60260; // 608:(608+320*8) ?
#endif
      //vrange = 0x07049025; // 0x025-0x124
      vrange = 0x07048c23; // (163-128):(163+128)
   } else {
      // should be NTSC
#ifdef CONFIG_VT_CONSOLE_HIRES
      mode=0x08000003; // 640w, 240p, no-PAL, 15bpp
      hrange = 0x06c60260; // 608:(608+320*8)
#else
      mode=0x08000001; // 320w, 240p, no-PAL, 15bpp
      hrange = 0x06c60260; // 608:(608+320*8) ?
#endif
      vrange = 0x07040010; // (136-120):(136+120)
   }

   InitGPU (mode, hrange, vrange);
   cls ();
   LoadFont ();
    psxvga_bottom = 0;

   return  display_desc;
}


static void psxvga_init(struct vc_data *conp, int init)
{

	conp->vc_can_do_color = 0;
	conp->vc_cols = PSXVGA_VSCR_W;
	conp->vc_rows = PSXVGA_VSCR_H;
	conp->vc_x=(unsigned int)psxvga_curx;
	conp->vc_y=(unsigned int)psxvga_cury;
	conp->vc_origin=(unsigned long)0;
	conp->vc_complement_mask=0x7700;
	conp->vc_size_row=PSXVGA_VSCR_W*2;
}


static void psxvga_deinit(struct vc_data *conp)
{

}

static inline void psxvga_writew2 (unsigned int val, int y, int x)
{
   if (y < PSXVGA_VSCR_H && x < PSXVGA_VSCR_W)
   {
	    //line(((y*PSXVGA_FNT_H)<<16)+((x*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x000100);
	   gpu_dma_gpu_idle();                            
	   print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, val);
      
      y += psxvga_bottom;
      if (y >= PSXVGA_VSCR_H) y -= PSXVGA_VSCR_H;
      psxvga_scrbuf[y][x] = val;
   }
}

static inline void psxvga_printscreen (void)
{
   int x, y, z;
   
	cls ();
	
	for (y = 0, z = psxvga_bottom; z < PSXVGA_SCR_H; z++, y++) // print top piece of screen
	{
      for(x = 0; x < PSXVGA_SCR_W; x++)
	   {
	      if (psxvga_scrbuf[z][x] != 0 && psxvga_scrbuf[z][x] != ' ')
	      {
	         gpu_dma_gpu_idle();
	         print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, psxvga_scrbuf[z][x]);
	      }
	   }
   }

	for (z = 0; z < psxvga_bottom; z++, y++) // print bottom piece of screen
	{
      for (x = 0; x < PSXVGA_SCR_W; x++)
	   {
	      if (psxvga_scrbuf[z][x] != 0 && psxvga_scrbuf[z][x] != ' ')
	      {
	         gpu_dma_gpu_idle();
	         print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, psxvga_scrbuf[z][x]);
	      }
	   }
	}
}


static inline u16 psxvga_readw (u16 addr)
{
   int x, y;
   
   y = addr/PSXVGA_VSCR_W;
   x = addr-y*PSXVGA_VSCR_W;

   return (u16)psxvga_scrbuf[y][x];
}

static inline void psxvga_memsetw(u16 sx,u16 sy, u16 c, unsigned int count)
{
   while (count) {
	   count--;
	   psxvga_writew2 (c, sy,sx);
   }
}


static inline void psxvga_memmovew(u16 to, u16 from, int count)
{
/*
    if (to < from) {
	while (count) {
	    count--;
	    psxvga_writew(psxvga_readw(from++), to++);
	}
    } else {
	from += count;
	to += count;
	while (count) {
	    count--;
	    psxvga_writew(psxvga_readw(--from), --to);
	}
    }
*/
}

/* ====================================================================== */

/*  fbcon_XXX routines - interface used by the world
 *
 *  This system is now divided into two levels because of complications
 *  caused by hardware scrolling. Top level functions:
 *
 *	fbcon_bmove(), fbcon_clear(), fbcon_putc()
 *
 *  handles y values in range [0, scr_height-1] that correspond to real
 *  screen positions. y_wrap shift means that first line of bitmap may be
 *  anywhere on this display. These functions convert lineoffsets to
 *  bitmap offsets and deal with the wrap-around case by splitting blits.
 *
 *	fbcon_bmove_physical_8()    -- These functions fast implementations
 *	fbcon_clear_physical_8()    -- of original fbcon_XXX fns.
 *	fbcon_putc_physical_8()	    -- (fontwidth != 8) may be added later
 *
 *  WARNING:
 *
 *  At the moment fbcon_putc() cannot blit across vertical wrap boundary
 *  Implies should only really hardware scroll in rows. Only reason for
 *  restriction is simplicity & efficiency at the moment.
 */

static void psxvga_clear(struct vc_data *conp, int sy, int sx, int height,
			int width)
{

int y;
 if((sy>PSXVGA_VSCR_H)){}
 else
  {
 for(y=0;y<height;y++)
  psxvga_memsetw(sx,(sy+y)*PSXVGA_VSCR_W,' ', width);
// XXX: no need to print2??
  }
 
}


static void psxvga_putc(struct vc_data *conp, int c, int ypos, int xpos)
{
   psxvga_writew2 (c, ypos, xpos);
}


static void psxvga_putcs(struct vc_data *conp, const unsigned short * s, int count,
		       int ypos, int xpos)
{
   int i;

   for (i = 0; i < count; i++)
	{
      psxvga_writew2 ((scr_readw(s++)), ypos, xpos+i);
   }
}


static void psxvga_cursor(struct vc_data *conp, int mode)
{int x,y,t;

y=psxvga_cury;
x=psxvga_curx;
//line(((y*PSXVGA_FNT_H)<<16)+(((x)*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x000100);
    t=y;
      y += psxvga_bottom;
      if (y >= PSXVGA_VSCR_H) y -= PSXVGA_VSCR_H;
    
gpu_dma_gpu_idle();
print2 ((x)*PSXVGA_FNT_W, (t)*PSXVGA_FNT_H, psxvga_scrbuf[y][x]);

x=conp->vc_x;
y=conp->vc_y;
gpu_dma_gpu_idle();
line(((y*PSXVGA_FNT_H)<<16)+((x*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x1122FF);
psxvga_cury=y;
psxvga_curx=x;

}


#define	TAG(next,size)	(((size) << 24) | ((u32)(next) & 0x00FFffff))
#define	END_TAG(size)	TAG(0xFFffff,size)
#define	CLUT_ID(page, x, y)	(( page & 0xf ) << 2 )  | (( page & 0x10 ) << 10 ) | ( x >> 4 ) | ( y << 6 )
// w and h must be <=256 (or wrapped)
static void blit(u32 syx, u32 dyx, u32 hw) {
	static u32 lis2[] = {
		END_TAG(0x04),
		0x65000000, // rectrawtexvar+bgr
		0x00000000, // yx
		(CLUT_ID( 21, 0, 128 ) << 16) | 0x0000, // clut+vu
		0x00000000, // hw
	};
	static u32 lis1[] = {
		0, //TAG(&lis2, 0x01),
		0xE1000500, // texpage: drawToDispArea, 15bpptex, y0*256, x0*64
	};

	u32 sy = syx >> 16;
	u32 sx = syx & 0xFFFF;
	u32 dy = dyx >> 16;
	u32 dx = dyx & 0xFFFF;
	u32 h = hw >> 16;
	u32 w = hw & 0xFFFF;

	lis1[0] = TAG(&lis2, 0x01);
	lis1[1] = 0xE1000500 | ((sy >> 8) << 4) | (sx >> 6);
	lis2[2] = (dy << 16) | dx;
	lis2[3] = ((CLUT_ID( 21, 0, 128 )) << 16) | ((sy & 0xFF) << 8) | (sx & 0x3F);
	lis2[4] = ((u32)h << 16) | w;

	gpu_dma_gpu_idle();
	SendList(&lis1);
}

static int psxvga_scroll(struct vc_data *conp, int t, int b, int dir, int count)
{
   int x, y, i;
   
   switch (dir)
   {
      case SM_UP:
			// hide cursor before scrolling
			y=psxvga_cury;
			x=psxvga_curx;
			i=y;
			y += psxvga_bottom;
			if (y >= PSXVGA_VSCR_H) y -= PSXVGA_VSCR_H;
			gpu_dma_gpu_idle();
			print2 ((x)*PSXVGA_FNT_W, (i)*PSXVGA_FNT_H, psxvga_scrbuf[y][x]);

		   for (y = psxvga_bottom, i = 0; i < count; i++, y++) {
            if (y >= PSXVGA_VSCR_H) y = 0;
		      for (x = 0; x < PSXVGA_VSCR_W; x++)
		         psxvga_scrbuf[y][x] = 0;
         }      
         psxvga_bottom += count;
	      if (psxvga_bottom >= PSXVGA_VSCR_H)
            psxvga_bottom -= PSXVGA_VSCR_H;

			// TODO t,b?
			gpu_dma_gpu_idle();
			//blit(0, count * PSXVGA_FNT_H, 0, 0, PSXVGA_VSCR_W * PSXVGA_FNT_W, (PSXVGA_VSCR_H - count) * PSXVGA_FNT_H);
			{
				const u32 syp = (count * PSXVGA_FNT_H);
				const u32 hp = ((PSXVGA_VSCR_H - count) * PSXVGA_FNT_H);
				blit(  0 | (syp << 16),   0 | (0 << 16),                                 256  | (hp << 16));
				blit(256 | (syp << 16), 256 | (0 << 16),                                 256  | (hp << 16));
				blit(512 | (syp << 16), 512 | (0 << 16), (PSXVGA_VSCR_W * PSXVGA_FNT_W - 512) | (hp << 16));
			}
			{
				const u32 hp = (PSXVGA_FNT_H * count);
				line(0 | ((PSXVGA_VSCR_H * PSXVGA_FNT_H - hp) << 16), (PSXVGA_VSCR_W * PSXVGA_FNT_W) | (hp << 16), 0x080000);
			}

			// show cursor after scroll
			y=psxvga_cury;
			x=psxvga_curx;
			gpu_dma_gpu_idle();
			line(((y*PSXVGA_FNT_H)<<16)+((x*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x1122FF);

         break;
         
      case SM_DOWN:
	 
         break;
   }
   
	//psxvga_printscreen ();
//	scrup();
   return 0;
}


static void psxvga_bmove(struct vc_data *conp, int sy, int sx, int dy, int dx,
			int height, int width)
{

   int y;
 if((sy>PSXVGA_VSCR_H)&&(dy>PSXVGA_VSCR_H)){}
 else
  {
 for(y=0;y<height;y++)
  psxvga_memmovew(sx+(sy+y)*PSXVGA_VSCR_W,dx+(dy+y)*PSXVGA_VSCR_W , width);
  }

}


static int psxvga_switch(struct vc_data *conp)
{
    return 1;
}


static int psxvga_blank(struct vc_data *conp, int blank)
{
    return 0;
}


static int psxvga_font_op(struct vc_data *conp, struct console_font_op *op)
{
	    return -ENOSYS;
}

static int psxvga_set_palette(struct vc_data *conp, unsigned char *table)
{
 return -EINVAL;
}

static u16 *psxvga_screen_pos(struct vc_data *conp, int offset)
{
    return (u16 *)(psxvga_cury+offset);
}

static unsigned long psxvga_getxy(struct vc_data *conp, unsigned long pos, int *px, int *py)
{unsigned long ret;
    
    if (px) *px = psxvga_curx;
    if (py) *py = psxvga_cury;
    ret = pos + (PSXVGA_VSCR_W - psxvga_curx) * 2; // WARNING !!!!
    
    return ret;
}

static void psxvga_invert_region(struct vc_data *conp, u16 *p, int cnt)
{

}

static int psxvga_scrolldelta(struct vc_data *conp, int lines)
{
    return 0;
}

static int psxvga_set_origin(struct vc_data *conp)
{
    return 0;
}

static void psxvga_save_screen(struct vc_data *conp)
{

}
static u8 psxvga_build_attr(struct vc_data *conp,u8 color, u8 intens, u8 blink, u8 underline, u8 reverse)
{
  u8 attr = color;
 return attr;
}

/*
 *  The console `switch' structure for the PSX GPU based console
 */
 
const struct consw psxvga_con = {
    con_startup: 	psxvga_startup, 
    con_init: 		psxvga_init,
    con_deinit: 	psxvga_deinit,
    con_clear: 		psxvga_clear,
    con_putc: 		psxvga_putc,
    con_putcs: 		psxvga_putcs,
    con_cursor: 	psxvga_cursor,
    con_scroll: 	psxvga_scroll,
    con_bmove: 		psxvga_bmove,
    con_switch: 	psxvga_switch,
    con_blank: 		psxvga_blank,
    con_font_op:	psxvga_font_op,
    con_set_palette: 	psxvga_set_palette,
    con_scrolldelta: 	psxvga_scrolldelta,
    con_set_origin: 	psxvga_set_origin,
    con_save_screen:	psxvga_save_screen,
    con_build_attr:	psxvga_build_attr,
    con_invert_region:	psxvga_invert_region,
    con_screen_pos:	psxvga_screen_pos,
    con_getxy:		psxvga_getxy
};


/*
 *  Dummy Low Level Operations
 */

static void psxvga_dummy_op(void) {}

#define DUMMY	(void *)psxvga_dummy_op
/*
struct display_switch psxvga_dummy = {
    setup:	DUMMY,
    bmove:	DUMMY,
    clear:	DUMMY,
    putc:	DUMMY,
    putcs:	DUMMY,
    revc:	DUMMY,
};
*/

/*
 *  Visible symbols for modules
 */

EXPORT_SYMBOL(psxvga_redraw_bmove);
EXPORT_SYMBOL(psxvga_redraw_clear);
EXPORT_SYMBOL(psxvga_dummy);
EXPORT_SYMBOL(psxvga_con);



