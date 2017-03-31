/* linux/arch/arm/plat-s3c24xx/s3c2443-clock.c
 *
 * Copyright (c) 2007, 2010 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * S3C2443 Clock control suport - common code
 */

#include <linux/init.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <mach/regs-s3c2443-clock.h>

#include <plat/s3c2443.h>
#include <plat/clock.h>
#include <plat/clock-clksrc.h>
#include <plat/cpu.h>

#include <plat/cpu-freq.h>


static int s3c2443_gate(void __iomem *reg, struct clk *clk, int enable)
{
	u32 ctrlbit = clk->ctrlbit;
	u32 con = __raw_readl(reg);

	if (enable)
		con |= ctrlbit;
	else
		con &= ~ctrlbit;

	__raw_writel(con, reg);
	return 0;
}

int s3c2443_clkcon_enable_h(struct clk *clk, int enable)
{
	return s3c2443_gate(S3C2443_HCLKCON, clk, enable);
}

int s3c2443_clkcon_enable_p(struct clk *clk, int enable)
{
	return s3c2443_gate(S3C2443_PCLKCON, clk, enable);
}

int s3c2443_clkcon_enable_s(struct clk *clk, int enable)
{
	return s3c2443_gate(S3C2443_SCLKCON, clk, enable);
}

/* mpllref is a direct descendant of clk_xtal by default, but it is not
 * elided as the EPLL can be either sourced by the XTAL or EXTCLK and as
 * such directly equating the two source clocks is impossible.
 */
struct clk clk_mpllref = {
	.name		= "mpllref",
	.parent		= &clk_xtal,
};

static struct clk *clk_epllref_sources[] = {
	[0] = &clk_mpllref,
	[1] = &clk_mpllref,
	[2] = &clk_xtal,
	[3] = &clk_ext,
};

struct clksrc_clk clk_epllref = {
	.clk	= {
		.name		= "epllref",
	},
	.sources = &(struct clksrc_sources) {
		.sources = clk_epllref_sources,
		.nr_sources = ARRAY_SIZE(clk_epllref_sources),
	},
	.reg_src = { .reg = S3C2443_CLKSRC, .size = 2, .shift = 7 },
};

/* esysclk
 *
 * this is sourced from either the EPLL or the EPLLref clock
*/

static struct clk *clk_sysclk_sources[] = {
	[0] = &clk_epllref.clk,
	[1] = &clk_epll,
};

struct clksrc_clk clk_esysclk = {
	.clk	= {
		.name		= "esysclk",
		.parent		= &clk_epll,
	},
	.sources = &(struct clksrc_sources) {
		.sources = clk_sysclk_sources,
		.nr_sources = ARRAY_SIZE(clk_sysclk_sources),
	},
	.reg_src = { .reg = S3C2443_CLKSRC, .size = 1, .shift = 6 },
};

static unsigned long s3c2443_getrate_mdivclk(struct clk *clk)
{
	unsigned long parent_rate = clk_get_rate(clk->parent);
	unsigned long div = __raw_readl(S3C2443_CLKDIV0);

	div  &= S3C2443_CLKDIV0_EXTDIV_MASK;
	div >>= (S3C2443_CLKDIV0_EXTDIV_SHIFT-1);	/* x2 */

	return parent_rate / (div + 1);
}

static struct clk clk_mdivclk = {
	.name		= "mdivclk",
	.parent		= &clk_mpllref,
	.ops		= &(struct clk_ops) {
		.get_rate	= s3c2443_getrate_mdivclk,
	},
};

static struct clk *clk_msysclk_sources[] = {
	[0] = &clk_mpllref,
	[1] = &clk_mpll,
	[2] = &clk_mdivclk,
	[3] = &clk_mpllref,
};

struct clksrc_clk clk_msysclk = {
	.clk	= {
		.name		= "msysclk",
		.parent		= &clk_xtal,
	},
	.sources = &(struct clksrc_sources) {
		.sources = clk_msysclk_sources,
		.nr_sources = ARRAY_SIZE(clk_msysclk_sources),
	},
	.reg_src = { .reg = S3C2443_CLKSRC, .size = 2, .shift = 3 },
};

/* prediv
 *
 * this divides the msysclk down to pass to h/p/etc.
 */

static unsigned long s3c2443_prediv_getrate(struct clk *clk)
{
	unsigned long rate = clk_get_rate(clk->parent);
	unsigned long clkdiv0 = __raw_readl(S3C2443_CLKDIV0);

	clkdiv0 &= S3C2443_CLKDIV0_PREDIV_MASK;
	clkdiv0 >>= S3C2443_CLKDIV0_PREDIV_SHIFT;

	return rate / (clkdiv0 + 1);
}

static struct clk clk_prediv = {
	.name		= "prediv",
	.parent		= &clk_msysclk.clk,
	.ops		= &(struct clk_ops) {
		.get_rate	= s3c2443_prediv_getrate,
	},
};

/* armdiv
 *
 * this clock is sourced from msysclk and can have a number of
 * divider values applied to it to then be fed into armclk.
*/

static unsigned int *armdiv;
static int nr_armdiv;
static int armdivmask;

static unsigned long s3c2443_armclk_roundrate(struct clk *clk,
					      unsigned long rate)
{
	unsigned long parent = clk_get_rate(clk->parent);
	unsigned long calc;
	unsigned best = 256; /* bigger than any value */
	unsigned div;
	int ptr;

	if (!nr_armdiv)
		return -EINVAL;

	for (ptr = 0; ptr < nr_armdiv; ptr++) {
		div = armdiv[ptr];
		if (div) {
			/* cpufreq provides 266mhz as 266666000 not 266666666 */
			calc = (parent / div / 1000) * 1000;
			if (calc <= rate && div < best)
				best = div;
		}
	}

	return parent / best;
}

static unsigned long s3c2443_armclk_getrate(struct clk *clk)
{
	unsigned long rate = clk_get_rate(clk->parent);
	unsigned long clkcon0;
	int val;

	if (!nr_armdiv || !armdivmask)
		return -EINVAL;

	clkcon0 = __raw_readl(S3C2443_CLKDIV0);
	clkcon0 &= armdivmask;
	val = clkcon0 >> S3C2443_CLKDIV0_ARMDIV_SHIFT;

	return rate / armdiv[val];
}

static int s3c2443_armclk_setrate(struct clk *clk, unsigned long rate)
{
	unsigned long parent = clk_get_rate(clk->parent);
	unsigned long calc;
	unsigned div;
	unsigned best = 256; /* bigger than any value */
	int ptr;
	int val = -1;

	if (!nr_armdiv || !armdivmask)
		return -EINVAL;

	for (ptr = 0; ptr < nr_armdiv; ptr++) {
		div = armdiv[ptr];
		if (div) {
			/* cpufreq provides 266mhz as 266666000 not 266666666 */
			calc = (parent / div / 1000) * 1000;
			if (calc <= rate && div < best) {
				best = div;
				val = ptr;
			}
		}
	}

	if (val >= 0) {
		unsigned long clkcon0;

		clkcon0 = __raw_readl(S3C2443_CLKDIV0);
		clkcon0 &= ~armdivmask;
		clkcon0 |= val << S3C2443_CLKDIV0_ARMDIV_SHIFT;
		__raw_writel(clkcon0, S3C2443_CLKDIV0);
	}

	return (val == -1) ? -EINVAL : 0;
}

static struct clk clk_armdiv = {
	.name		= "armdiv",
	.parent		= &clk_msysclk.clk,
	.ops		= &(struct clk_ops) {
		.round_rate = s3c2443_armclk_roundrate,
		.get_rate = s3c2443_armclk_getrate,
		.set_rate = s3c2443_armclk_setrate,
	},
};

/* armclk
 *
 * this is the clock fed into the ARM core itself, from armdiv or from hclk.
 */

static struct clk *clk_arm_sources[] = {
	[0] = &clk_armdiv,
	[1] = &clk_h,
};

static struct clksrc_clk clk_arm = {
	.clk	= {
		.name		= "armclk",
	},
	.sources = &(struct clksrc_sources) {
		.sources = clk_arm_sources,
		.nr_sources = ARRAY_SIZE(clk_arm_sources),
	},
	.reg_src = { .reg = S3C2443_CLKDIV0, .size = 1, .shift = 13 },
};

/* usbhost
 *
 * usb host bus-clock, usually 48MHz to provide USB bus clock timing
*/

static struct clksrc_clk clk_usb_bus_host = {
	.clk	= {
		.name		= "usb-bus-host-parent",
		.parent		= &clk_esysclk.clk,
		.ctrlbit	= S3C2443_SCLKCON_USBHOST,
		.enable		= s3c2443_clkcon_enable_s,
	},
	.reg_div = { .reg = S3C2443_CLKDIV1, .size = 2, .shift = 4 },
};

/* common clksrc clocks */

static struct clksrc_clk clksrc_clks[] = {
	{
		/* ART baud-rate clock sourced from esysclk via a divisor */
		.clk	= {
			.name		= "uartclk",
			.parent		= &clk_esysclk.clk,
		},
		.reg_div = { .reg = S3C2443_CLKDIV1, .size = 4, .shift = 8 },
	}, {
		/* camera interface bus-clock, divided down from esysclk */
		.clk	= {
			.name		= "camif-upll",	/* same as 2440 name */
			.parent		= &clk_esysclk.clk,
			.ctrlbit	= S3C2443_SCLKCON_CAMCLK,
			.enable		= s3c2443_clkcon_enable_s,
		},
		.reg_div = { .reg = S3C2443_CLKDIV1, .size = 4, .shift = 26 },
	}, {
		.clk	= {
			.name		= "display-if",
			.parent		= &clk_esysclk.clk,
			.ctrlbit	= S3C2443_SCLKCON_DISPCLK,
			.enable		= s3c2443_clkcon_enable_s,
		},
		.reg_div = { .reg = S3C2443_CLKDIV1, .size = 8, .shift = 16 },
	},
};

static struct clk clk_i2s_ext = {
	.name		= "i2s-ext",
};

/* i2s_eplldiv
 *
 * This clock is the output from the I2S divisor of ESYSCLK, and is separate
 * from the mux that comes after it (cannot merge into one single clock)
*/

static struct clksrc_clk clk_i2s_eplldiv = {
	.clk	= {
		.name		= "i2s-eplldiv",
		.parent		= &clk_esysclk.clk,
	},
	.reg_div = { .reg = S3C2443_CLKDIV1, .size = 4, .shift = 12, },
};

/* i2s-ref
 *
 * i2s bus reference clock, selectable from external, esysclk or epllref
 *
 * Note, this used to be two clocks, but was compressed into one.
*/

static struct clk *clk_i2s_srclist[] = {
	[0] = &clk_i2s_eplldiv.clk,
	[1] = &clk_i2s_ext,
	[2] = &clk_epllref.clk,
	[3] = &clk_epllref.clk,
};

static struct clksrc_clk clk_i2s = {
	.clk	= {
		.name		= "i2s-if",
		.ctrlbit	= S3C2443_SCLKCON_I2SCLK,
		.enable		= s3c2443_clkcon_enable_s,

	},
	.sources = &(struct clksrc_sources) {
		.sources = clk_i2s_srclist,
		.nr_sources = ARRAY_SIZE(clk_i2s_srclist),
	},
	.reg_src = { .reg = S3C2443_CLKSRC, .size = 2, .shift = 14 },
};

static struct clk init_clocks_off[] = {
	{
		.name		= "iis",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_IIS,
	}, {
		.name		= "hsspi",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_HSSPI,
	}, {
		.name		= "adc",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_ADC,
	}, {
		.name		= "i2c",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_IIC,
	}
};

static struct clk init_clocks[] = {
	{
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA0,
	}, {
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA1,
	}, {
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA2,
	}, {
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA3,
	}, {
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA4,
	}, {
		.name		= "dma",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_DMA5,
	}, {
		.name		= "hsmmc",
		.devname	= "s3c-sdhci.1",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_HSMMC,
	}, {
		.name		= "gpio",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_GPIO,
	}, {
		.name		= "usb-host",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_USBH,
	}, {
		.name		= "usb-device",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_USBD,
	}, {
		.name		= "lcd",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_LCDC,

	}, {
		.name		= "timers",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_PWMT,
	}, {
		.name		= "cfc",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_CFC,
	}, {
		.name		= "ssmc",
		.parent		= &clk_h,
		.enable		= s3c2443_clkcon_enable_h,
		.ctrlbit	= S3C2443_HCLKCON_SSMC,
	}, {
		.name		= "uart",
		.devname	= "s3c2440-uart.0",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_UART0,
	}, {
		.name		= "uart",
		.devname	= "s3c2440-uart.1",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_UART1,
	}, {
		.name		= "uart",
		.devname	= "s3c2440-uart.2",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_UART2,
	}, {
		.name		= "uart",
		.devname	= "s3c2440-uart.3",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_UART3,
	}, {
		.name		= "rtc",
		.parent		= &clk_p,
		.enable		= s3c2443_clkcon_enable_p,
		.ctrlbit	= S3C2443_PCLKCON_RTC,
	}, {
		.name		= "watchdog",
		.parent		= &clk_p,
		.ctrlbit	= S3C2443_PCLKCON_WDT,
	}, {
		.name		= "ac97",
		.parent		= &clk_p,
		.ctrlbit	= S3C2443_PCLKCON_AC97,
	}, {
		.name		= "nand",
		.parent		= &clk_h,
	}, {
		.name		= "usb-bus-host",
		.parent		= &clk_usb_bus_host.clk,
	}
};

static inline unsigned long s3c2443_get_hdiv(unsigned long clkcon0)
{
	clkcon0 &= S3C2443_CLKDIV0_HCLKDIV_MASK;

	return clkcon0 + 1;
}

/* EPLLCON compatible enough to get on/off information */

void __init_or_cpufreq s3c2443_common_setup_clocks(pll_fn get_mpll)
{
	unsigned long epllcon = __raw_readl(S3C2443_EPLLCON);
	unsigned long mpllcon = __raw_readl(S3C2443_MPLLCON);
	unsigned long clkdiv0 = __raw_readl(S3C2443_CLKDIV0);
	struct clk *xtal_clk;
	unsigned long xtal;
	unsigned long pll;
	unsigned long fclk;
	unsigned long hclk;
	unsigned long pclk;
	int ptr;

	xtal_clk = clk_get(NULL, "xtal");
	xtal = clk_get_rate(xtal_clk);
	clk_put(xtal_clk);

	pll = get_mpll(mpllcon, xtal);
	clk_msysclk.clk.rate = pll;

	fclk = clk_get_rate(&clk_armdiv);
	hclk = s3c2443_prediv_getrate(&clk_prediv);
	hclk /= s3c2443_get_hdiv(clkdiv0);
	pclk = hclk / ((clkdiv0 & S3C2443_CLKDIV0_HALF_PCLK) ? 2 : 1);

	s3c24xx_setup_clocks(fclk, hclk, pclk);

	printk("CPU: MPLL %s %ld.%03ld MHz, cpu %ld.%03ld MHz, mem %ld.%03ld MHz, pclk %ld.%03ld MHz\n",
	       (mpllcon & S3C2443_PLLCON_OFF) ? "off":"on",
	       print_mhz(pll), print_mhz(fclk),
	       print_mhz(hclk), print_mhz(pclk));

	for (ptr = 0; ptr < ARRAY_SIZE(clksrc_clks); ptr++)
		s3c_set_clksrc(&clksrc_clks[ptr], true);

	/* ensure usb bus clock is within correct rate of 48MHz */

	if (clk_get_rate(&clk_usb_bus_host.clk) != (48 * 1000 * 1000)) {
		printk(KERN_INFO "Warning: USB host bus not at 48MHz\n");
		clk_set_rate(&clk_usb_bus_host.clk, 48*1000*1000);
	}

	printk("CPU: EPLL %s %ld.%03ld MHz, usb-bus %ld.%03ld MHz\n",
	       (epllcon & S3C2443_PLLCON_OFF) ? "off":"on",
	       print_mhz(clk_get_rate(&clk_epll)),
	       print_mhz(clk_get_rate(&clk_usb_bus)));
}

static struct clk *clks[] __initdata = {
	&clk_prediv,
	&clk_mpllref,
	&clk_mdivclk,
	&clk_ext,
	&clk_epll,
	&clk_usb_bus,
	&clk_armdiv,
};

static struct clksrc_clk *clksrcs[] __initdata = {
	&clk_i2s_eplldiv,
	&clk_i2s,
	&clk_usb_bus_host,
	&clk_epllref,
	&clk_esysclk,
	&clk_msysclk,
	&clk_arm,
};

void __init s3c2443_common_init_clocks(int xtal, pll_fn get_mpll,
				       unsigned int *divs, int nr_divs,
				       int divmask)
{
	int ptr;

	armdiv = divs;
	nr_armdiv = nr_divs;
	armdivmask = divmask;

	/* s3c2443 parents h and p clocks from prediv */
	clk_h.parent = &clk_prediv;
	clk_p.parent = &clk_prediv;

	clk_usb_bus.parent = &clk_usb_bus_host.clk;
	clk_epll.parent = &clk_epllref.clk;

	s3c24xx_register_baseclocks(xtal);
	s3c24xx_register_clocks(clks, ARRAY_SIZE(clks));

	for (ptr = 0; ptr < ARRAY_SIZE(clksrcs); ptr++)
		s3c_register_clksrc(clksrcs[ptr], 1);

	s3c_register_clksrc(clksrc_clks, ARRAY_SIZE(clksrc_clks));
	s3c_register_clocks(init_clocks, ARRAY_SIZE(init_clocks));

	/* See s3c2443/etc notes on disabling clocks at init time */
	s3c_register_clocks(init_clocks_off, ARRAY_SIZE(init_clocks_off));
	s3c_disable_clocks(init_clocks_off, ARRAY_SIZE(init_clocks_off));

	s3c2443_common_setup_clocks(get_mpll);
}