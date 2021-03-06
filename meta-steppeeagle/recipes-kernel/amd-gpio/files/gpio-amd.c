/*****************************************************************************
*
* Copyright (c) 2014, Advanced Micro Devices, Inc.   
* All rights reserved.   
*
* Redistribution and use in source and binary forms, with or without   
* modification, are permitted provided that the following conditions are met:   
*     * Redistributions of source code must retain the above copyright   
*       notice, this list of conditions and the following disclaimer.   
*     * Redistributions in binary form must reproduce the above copyright   
*       notice, this list of conditions and the following disclaimer in the   
*       documentation and/or other materials provided with the distribution.   
*     * Neither the name of Advanced Micro Devices, Inc. nor the names of    
*       its contributors may be used to endorse or promote products derived    
*       from this software without specific prior written permission.   
*    
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND   
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED   
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE   
* DISCLAIMED. IN NO EVENT SHALL ADVANCED MICRO DEVICES, INC. BE LIABLE FOR ANY   
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES   
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;   
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND   
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT   
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS   
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.   
*    
*   
***************************************************************************/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/gpio.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/io.h>

#include <asm/io.h>

#include <linux/cdev.h>
#include <linux/fs.h>           /* everything... */
#include <asm/io.h>
#include <linux/ioctl.h>
#include <linux/device.h>

#include "gpio-amd.h"

static u32 gpiobase_phys;
static u32 iomuxbase_phys;
static struct pci_dev *amd_gpio_pci;
static struct platform_device *amd_gpio_platform_device;


static int dev_major;
static int dev_minor = 0;

static struct gpio_test_dev{
	struct cdev cdev;
	struct class *gpio_class;
}gpio_test_dev;


/* The following GPIO pins are reserved as per the specification. 184 max */
static u8 mask[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
	0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0,
	0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
	1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0,
};


static int gpio_mask[AMD_GPIO_NUM_PINS];
static unsigned int num_mask;
module_param_array(gpio_mask, int, &num_mask, 0);
MODULE_PARM_DESC(gpio_mask, "GPIO mask which marks them as reserved");

static int gpio_mode[AMD_GPIO_NUM_PINS];
static unsigned int num_modes;
module_param_array(gpio_mode, int, &num_modes, 0);
MODULE_PARM_DESC(gpio_mode, "Specifies whether the GPIO mentioned "
			"in gpio_mask is 0-reserved, 1-available, 2-GPI only, "
			"3-GPO only");

static struct amd_gpio_chip {
	struct gpio_chip gpio;

	void __iomem *gpiobase;
	void __iomem *iomuxbase;

	struct platform_device *pdev;
	spinlock_t lock;
} amd_gpio_chip;

static int amd_gpio_request(struct gpio_chip *c, unsigned offset)
{
	struct amd_gpio_chip *chip = container_of(c, struct amd_gpio_chip,
						  gpio);
	unsigned long flags;
	u8 iomux_reg;
	u32 gpio_reg = 0;

	spin_lock_irqsave(&chip->lock, flags);

	/* check if this pin is available */
	if (mask[offset] == AMD_GPIO_MODE_RESV) {
		spin_unlock_irqrestore(&chip->lock, flags);
		pr_info("GPIO pin %u not available\n", offset);
		return -EINVAL;
	}

	/* Program the GPIO Wake/Interrupt Switch offset is AMD_GPIO_MSWITCH */
	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + AMD_GPIO_MSWITCH);
	/* to disable all GPIO wake and interrupt*/
	gpio_reg &= (~AMD_GPIO_WAKE_EN & ~AMD_GPIO_INTERRUPT_EN);
	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + AMD_GPIO_MSWITCH));

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);
	/* clear wake status and interrupt status */
	gpio_reg |= (AMD_GPIO_INTERPT_STATUS | AMD_GPIO_WAKE_STATUS);

	/* Set disable both Pull Up and Pull Down and disable output */
	gpio_reg &= (~AMD_GPIO_PULLUP_ENABLE & ~AMD_GPIO_PULLDN_ENABLE
			& ~AMD_GPIO_DEB_CTRL & ~AMD_GPIO_SWCTRL_IN
			& ~AMD_GPIO_WAKECTRL & ~AMD_GPIO_INTERPT_ENABLE);
	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));

	/* Enable GPIO by writing to the corresponding IOMUX register */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + offset);
	iomux_reg &= ~AMD_IOMUX_GPIO_MASK;

	if(offset == 10 || offset == 72 || offset == 73 ||offset == 76)
		iomux_reg |= AMD_IOMUX_ENABLE_FUNC0;
	else if(offset == 35 || offset == 64 || offset == 65 ||offset == 66 ||
		offset == 71 || offset == 93 ||offset == 115 ||offset == 116)
		iomux_reg |= AMD_IOMUX_ENABLE_FUNC2;
	else if(offset == 67 || offset == 70)
		iomux_reg |= AMD_IOMUX_ENABLE_FUNC3;
	else
		iomux_reg |= AMD_IOMUX_ENABLE_FUNC1;

	iowrite8(iomux_reg, ((u8 *)amd_gpio_chip.iomuxbase + offset));

	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}


static int amd_gpio_get(struct gpio_chip *c, unsigned offset)
{
	struct amd_gpio_chip *chip = container_of(c, struct amd_gpio_chip,
						  gpio);
	unsigned long flags;
	u32 gpio_reg;

	spin_lock_irqsave(&chip->lock, flags);

	/* Read the GPIO register */
	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);

	spin_unlock_irqrestore(&chip->lock, flags);

	return (gpio_reg & AMD_GPIO_GET_INPUT) ? 1 : 0;
}

static void amd_gpio_set(struct gpio_chip *c, unsigned offset, int val)
{
	struct amd_gpio_chip *chip = container_of(c, struct amd_gpio_chip,
						  gpio);
	unsigned long flags;
	u32 gpio_reg;

	spin_lock_irqsave(&chip->lock, flags);

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);

	/* Set GPIO Output depending on 'val' */
	if (val)
		gpio_reg |= AMD_GPIO_SET_OUTPUT;
	else
		gpio_reg &= ~AMD_GPIO_SET_OUTPUT;

	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));

	spin_unlock_irqrestore(&chip->lock, flags);
}

static int amd_gpio_direction_input(struct gpio_chip *c, unsigned offset)
{
	struct amd_gpio_chip *chip = container_of(c, struct amd_gpio_chip,
						  gpio);
	unsigned long flags;
	u32 gpio_reg;

	spin_lock_irqsave(&chip->lock, flags);

	/* If the mask says the pin should be GPO, we return from here */
	if (mask[offset] == AMD_GPIO_MODE_OUTPUT) {
		pr_info("GPIO %u can only be set in output mode\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);
	/* Disable output by set the bit to 0 */
	gpio_reg &= ~AMD_GPIO_OUTPUT_ENABLE;
	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));

	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int amd_gpio_direction_output(struct gpio_chip *c, unsigned offset,
				     int val)
{
	struct amd_gpio_chip *chip = container_of(c, struct amd_gpio_chip,
						  gpio);
	unsigned long flags;
	u32 gpio_reg;

	spin_lock_irqsave(&chip->lock, flags);

	/* If the mask says the pin should be GPI, we return from here */
	if (mask[offset] == AMD_GPIO_MODE_INPUT) {
		pr_info("GPIO %u can only be set in input mode\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);


	gpio_reg |= AMD_GPIO_DRV_STRENGTH(2);
	/* Set disable both Pull Up and Pull Down */
	gpio_reg &= (~AMD_GPIO_PULLUP_ENABLE & ~AMD_GPIO_PULLDN_ENABLE
			& ~AMD_GPIO_DEB_CTRL & ~AMD_GPIO_SWCTRL_IN
			& ~AMD_GPIO_WAKECTRL & ~AMD_GPIO_INTERPT_ENABLE);
	/* Enable output */
	gpio_reg |= AMD_GPIO_OUTPUT_ENABLE;

	/* Set GPIO Output depending on 'val' */
	if (val)
		gpio_reg |= AMD_GPIO_SET_OUTPUT;
	else
		gpio_reg &= ~AMD_GPIO_SET_OUTPUT;

	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));

	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static struct amd_gpio_chip amd_gpio_chip = {
	.gpio = {
		.owner = THIS_MODULE,
		.label = DRV_NAME,

		.base = 0,
		.ngpio = AMD_GPIO_NUM_PINS,
		.names = NULL,
		.request = amd_gpio_request,
		.get = amd_gpio_get,
		.set = amd_gpio_set,
		.direction_input = amd_gpio_direction_input,
		.direction_output = amd_gpio_direction_output,
	},
};

/*
* The PCI Device ID table below is used to identify the platform
* the driver is supposed to work for. Since this is a platform
* driver, we need a way for us to be able to find the correct
* platform when the driver gets loaded, otherwise we should
* bail out.
*/
static DEFINE_PCI_DEVICE_TABLE(amd_gpio_pci_tbl) = {
	{ PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_HUDSON2_SMBUS, PCI_ANY_ID,
	  PCI_ANY_ID, },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, amd_gpio_pci_tbl);

static void amd_update_gpio_mask(void)
{
	u8 iomux_reg;

	/*
	 * Some of the GPIO pins have an alternate function assigned to
	 * them. That will be reflected in the corresponding IOMUX
	 * registers. If so, we mark these GPIO pins as reserved.
	 */

	/* AGPIO10 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x0A);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[10] = AMD_GPIO_MODE_RESV;

	/* EGPIO19 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x13);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[19] = AMD_GPIO_MODE_RESV;

	/* EGPIO20 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x14);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[20] = AMD_GPIO_MODE_RESV;

	/* EGPIO26 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1A);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[26] = AMD_GPIO_MODE_RESV;

	/* EGPIO27 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1B);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[27] = AMD_GPIO_MODE_RESV;

	/* EGPIO28 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1C);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[28] = AMD_GPIO_MODE_RESV;

	/* EGPIO29 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1D);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[29] = AMD_GPIO_MODE_RESV;

	/* EGPIO30 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1E);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[30] = AMD_GPIO_MODE_RESV;

	/* EGPIO31 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x1F);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[31] = AMD_GPIO_MODE_RESV;

	/* EGPIO35 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x23);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[35] = AMD_GPIO_MODE_RESV;

	/* AGPIO64 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x40);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[64] = AMD_GPIO_MODE_RESV;

	/* AGPIO65 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x41);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[65] = AMD_GPIO_MODE_RESV;

	/* AGPIO66 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x42);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[66] = AMD_GPIO_MODE_RESV;

	/* AGPIO67 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x43);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2))
		mask[67] = AMD_GPIO_MODE_RESV;

	/* AGPIO68 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x44);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[68] = AMD_GPIO_MODE_RESV;

	/* AGPIO69 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x45);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[68] = AMD_GPIO_MODE_RESV;

	/* AGPIO70 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x46);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2))
		mask[70] = AMD_GPIO_MODE_RESV;

	/* AGPIO71 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x47);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[71] = AMD_GPIO_MODE_RESV;

	/* AGPIO72 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x48);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[72] = AMD_GPIO_MODE_RESV;

	/* AGPIO73 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x49);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[73] = AMD_GPIO_MODE_RESV;

	/* AGPIO76 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x4C);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[76] = AMD_GPIO_MODE_RESV;

	/* AGPIO77 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x4D);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[77] = AMD_GPIO_MODE_RESV;

	/* EGPIO84 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x54);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[84] = AMD_GPIO_MODE_RESV;

	/* EGPIO85 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x55);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[85] = AMD_GPIO_MODE_RESV;

	/* EGPIO87 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x57);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[87] = AMD_GPIO_MODE_RESV;

	/* EGPIO88 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x58);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[88] = AMD_GPIO_MODE_RESV;

	/* AGPIO91 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x5B);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[91] = AMD_GPIO_MODE_RESV;

	/* EGPIO93 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x5D);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[93] = AMD_GPIO_MODE_RESV;

	/* EGPIO115 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x73);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[115] = AMD_GPIO_MODE_RESV;

	/* EGPIO116 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x74);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC1) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[116] = AMD_GPIO_MODE_RESV;

	/* AGPIO130 */
	iomux_reg = ioread8((u8 *)amd_gpio_chip.iomuxbase + 0x82);
	if (((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC0) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC2) ||
		((iomux_reg & AMD_IOMUX_GPIO_MASK) == AMD_IOMUX_ENABLE_FUNC3))
		mask[86] = AMD_GPIO_MODE_RESV;
}

static int amd_gpio_init(struct platform_device *pdev)
{
	struct pci_dev *dev = NULL;
	int i;
	int err;

	/* Match the PCI device */
	for_each_pci_dev(dev) {
		if (pci_match_id(amd_gpio_pci_tbl, dev) != NULL) {
			amd_gpio_pci = dev;
			break;
		}
	}

	if (!amd_gpio_pci)
		return -ENODEV;

	/* GPIO registers range from AMD_GPIO_ACPIMMIO_BASE+1500h to 
						AMD_GPIO_ACPIMMIO_BASE+17FFh. */
	if (!request_mem_region_exclusive(AMD_GPIO_ACPIMMIO_BASE
		+ AMD_GPIO_BANK_OFFSET,	AMD_GPIO_MEM_MAP_SIZE, "AMD GPIO")) {
		pr_err("mmio address 0x%04x already in use\n",
			AMD_GPIO_ACPIMMIO_BASE + AMD_GPIO_BANK_OFFSET );
		goto exit;
	}
	gpiobase_phys = AMD_GPIO_ACPIMMIO_BASE + AMD_GPIO_BANK_OFFSET;

	amd_gpio_chip.gpiobase = ioremap(gpiobase_phys, AMD_GPIO_MEM_MAP_SIZE);
	if (!amd_gpio_chip.gpiobase) {
		pr_err("failed to get gpiobase address\n");
		goto unreg_gpio_region;
	}

	/* IOMUX Base Address starts from ACPI MMIO Base Address + 0xD00 */
	if (!request_mem_region_exclusive(AMD_GPIO_ACPIMMIO_BASE
					+ AMD_IOMUX_MEM_MAP_OFFSET,
					AMD_IOMUX_MEM_MAP_SIZE,	"AMD IOMUX")) {
		pr_err("mmio address 0x%04x already in use\n",
			AMD_GPIO_ACPIMMIO_BASE + AMD_IOMUX_MEM_MAP_OFFSET);
		goto unmap_gpio_region;
	}
	iomuxbase_phys = AMD_GPIO_ACPIMMIO_BASE + AMD_IOMUX_MEM_MAP_OFFSET;

	amd_gpio_chip.iomuxbase = ioremap(iomuxbase_phys,
						AMD_IOMUX_MEM_MAP_SIZE);
	if (!amd_gpio_chip.iomuxbase) {
		pr_err("failed to get iomuxbase address\n");
		goto unreg_iomux_region;
	}

	/* Set up driver specific struct */
	amd_gpio_chip.pdev = pdev;
	spin_lock_init(&amd_gpio_chip.lock);

	/* Register ourself with the GPIO core */
	err = gpiochip_add(&amd_gpio_chip.gpio);
	if (err)
		goto unmap_iomux_region;

	/*
	 * Lets take care of special GPIO pins, and mark them as reserved
	 * as appropriate.
	 */
	amd_update_gpio_mask();

	/*
	 * If the number of GPIO pins provided during module loading does
	 * not match the number of GPIO modes, we fall back to the default
	 * mask.
	 */
	if (num_mask == num_modes) {
		/*
		 * If the number of masks or the number of modes specified
		 * is more than the maximum number of GPIO pins supported
		 * by the driver, we set the limit to the one supported
		 * driver.
		 */
		if (num_mask > AMD_GPIO_NUM_PINS)
			num_mask = num_modes = AMD_GPIO_NUM_PINS;

		/*
		 * The default mask is our de facto standard. The GPIO
		 * pins marked reserved in the default mask stay reserved
		 * no matter what the module load parameter says. Also, we
		 * set the mode of the GPIO pins depending on the value
		 * of gpio_mode provided.
		 */
		for (i = 0; i < num_mask; i++) {
			if (mask[gpio_mask[i]] != AMD_GPIO_MODE_RESV) {
				mask[gpio_mask[i]] = gpio_mode[i];

				/*
				 * gpio_request() can fail, in which case we
				 * won't set the GPIO modes.
				 */
				if(!gpio_request(gpio_mask[i], DRV_NAME)) {
					if (gpio_mode[i] ==
						AMD_GPIO_MODE_INPUT)
						gpio_direction_input(gpio_mask[i]);
					else if (gpio_mode[i] ==
						AMD_GPIO_MODE_OUTPUT)
						gpio_direction_output(gpio_mask[i],
									0);

					gpio_free(gpio_mask[i]);
				}
			}
		}
	}

	return 0;

unmap_iomux_region:
	iounmap(amd_gpio_chip.iomuxbase);
unreg_iomux_region:
	release_mem_region(iomuxbase_phys, AMD_IOMUX_MEM_MAP_SIZE);
unmap_gpio_region:
	iounmap(amd_gpio_chip.gpiobase);
unreg_gpio_region:
	release_mem_region(gpiobase_phys, AMD_GPIO_MEM_MAP_SIZE);
exit:
	return -ENODEV;
}

static int amd_gpio_remove(struct platform_device *pdev)
{
	gpiochip_remove(&amd_gpio_chip.gpio);
	iounmap(amd_gpio_chip.iomuxbase);
	release_mem_region(iomuxbase_phys, AMD_IOMUX_MEM_MAP_SIZE);
	iounmap(amd_gpio_chip.gpiobase);
	release_mem_region(gpiobase_phys, AMD_GPIO_MEM_MAP_SIZE);

	return 0;
}

static struct platform_driver amd_gpio_driver = {
	.probe		= amd_gpio_init,
	.remove		= amd_gpio_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= GPIO_MODULE_NAME,
	},
};

/* interface to debug driver for gpio */

static int amd_gpio_swctrlen(int offset, int value)
{
	struct amd_gpio_chip *chip = &amd_gpio_chip;
	u32 gpio_reg;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);

		/* If the mask says the pin should be GPO, we return from here */
	if (mask[offset] == AMD_GPIO_MODE_RESV) {
		pr_info("GPIO %u is reserved\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	if (mask[offset] == AMD_GPIO_MODE_OUTPUT) {
		pr_info("GPIO %u can only be set in output mode\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);
	/* Disable output by set the bit to 0 */
	gpio_reg &= ~AMD_GPIO_OUTPUT_ENABLE;
	/* enable or disable sw input */
	if(value)
		gpio_reg |= AMD_GPIO_SWCTRL_EN;
	else
		gpio_reg &= ~AMD_GPIO_SWCTRL_EN;

	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));

	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int amd_gpio_swctrlin(int offset, int value)
{
	struct amd_gpio_chip *chip = &amd_gpio_chip;
	u32 gpio_reg;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
		/* If the mask says the pin should be GPO, we return from here */
	if (mask[offset] == AMD_GPIO_MODE_RESV) {
		pr_info("GPIO %u is reserved\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	if (mask[offset] == AMD_GPIO_MODE_OUTPUT) {
		pr_info("GPIO %u can only be set in input mode\n", offset);
		spin_unlock_irqrestore(&chip->lock, flags);
		return -EINVAL;
	}

	gpio_reg = ioread32((u32 *)amd_gpio_chip.gpiobase + offset);
	/* Disable output by set the bit to 0 */
	gpio_reg &= ~AMD_GPIO_OUTPUT_ENABLE;
	/* enable or disable sw input */
	if(value)
		gpio_reg |= AMD_GPIO_SWCTRL_IN;
	else
		gpio_reg &= ~AMD_GPIO_SWCTRL_IN;

	iowrite32(gpio_reg, ((u32 *)amd_gpio_chip.gpiobase + offset));
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}


static int gpio_test_open(struct inode *inode, struct file *filp)
{
	struct gpio_test_dev *dev; /* device information */

	dev = container_of(inode->i_cdev, struct gpio_test_dev, cdev);
	filp->private_data = dev; /* for other methods */

	return 0;          /* success */
}

static int gpio_test_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long gpio_test_ioctl(struct file *filp,
                 unsigned cmd, unsigned long arg)
{

	debug_data  tmp;
	unsigned long retval = 0;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != GPIO_TEST_IOC_MAGIC)
		return -ENOTTY;

	retval = copy_from_user(&tmp, (void __user *)arg, sizeof(debug_data));
	if(retval != 0)
	return 0;

	switch(cmd) {
		case GPIO_IOC_SWCTRLIN:
			retval = amd_gpio_swctrlin(tmp.offset,tmp.value);
		break;
		case GPIO_IOC_SWCTRLEN:
			retval = amd_gpio_swctrlen(tmp.offset,tmp.value);
		break;
		default:
			return -ENOTTY;
	}

	return retval;
}

struct file_operations gpio_test_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gpio_test_ioctl,
	.open = gpio_test_open,
	.release = gpio_test_release,
};


static int __init amd_gpio_init_module(void)
{
	int err;
	dev_t dev = 0;
	struct device *gpio_device = 0;
	
	pr_info("AMD GPIO Driver v%s\n", GPIO_VERSION);

	err = platform_driver_register(&amd_gpio_driver);
	if (err)
		return err;

	err = alloc_chrdev_region(&dev, dev_minor, 1,"gpio_test_driver");
	if (err < 0) {
		printk(KERN_WARNING " can't get major %d\n", dev_major);
		goto unreg_platform_driver;
	}

	gpio_test_dev.gpio_class = class_create(THIS_MODULE, "gpio_test_driver");
	if (IS_ERR(gpio_test_dev.gpio_class)) {
		printk(" error in class create \n");
		goto unregister_test_driver;
	}

	dev_major = MAJOR(dev);

	gpio_device = device_create(gpio_test_dev.gpio_class, NULL, dev, NULL,
				    "gpio_test_driver");
	if (IS_ERR(gpio_device)) {
		printk(" error in device create \n");
		goto destroy_class;
	}

	cdev_init(&gpio_test_dev.cdev, &gpio_test_fops);
	err = cdev_add (&gpio_test_dev.cdev, dev, 1);
	if(err)
		goto destroy_device;

	amd_gpio_platform_device = platform_device_register_simple(
					GPIO_MODULE_NAME, -1, NULL, 0);
	if (IS_ERR(amd_gpio_platform_device)) {
		err = PTR_ERR(amd_gpio_platform_device);
		goto cdev_delete;
	}

	return 0;

cdev_delete:
	cdev_del(&gpio_test_dev.cdev);
destroy_device:
	device_destroy(gpio_test_dev.gpio_class, dev);
destroy_class:
	class_destroy(gpio_test_dev.gpio_class);
unregister_test_driver:
	unregister_chrdev_region(dev, 1);
unreg_platform_driver:
	platform_driver_unregister(&amd_gpio_driver);
	return err;
}

static void __exit amd_gpio_cleanup_module(void)
{
	dev_t dev = MKDEV(dev_major, dev_minor);
	
	device_destroy(gpio_test_dev.gpio_class, dev);
	class_destroy(gpio_test_dev.gpio_class);
	cdev_del(&gpio_test_dev.cdev);
	unregister_chrdev_region(dev, 1);
	platform_device_unregister(amd_gpio_platform_device);
	platform_driver_unregister(&amd_gpio_driver);
	pr_info("AMD GPIO Module Unloaded\n");
}

module_init(amd_gpio_init_module);
module_exit(amd_gpio_cleanup_module);



MODULE_AUTHOR("Sudheesh Mavila <sudheesh.mavila@amd.com>");
MODULE_DESCRIPTION("GPIO driver for AMD chipsets");
MODULE_LICENSE("Dual BSD/GPL");
