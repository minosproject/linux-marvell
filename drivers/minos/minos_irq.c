/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>

#define SPI_IRQ_BASE 32

static struct platform_device *mpdev;

int get_dynamic_virq(int irq)
{
	struct device_node *node;

	if (!mpdev) {
		node = of_find_node_by_name(of_root, "vm_fake_device");
		if (!node)
			return 0;

		mpdev = of_find_device_by_node(node);
	}

	if (!mpdev) {
		pr_err("fake device not init\n");
		return 0;
	}

	return platform_get_irq(mpdev, irq - SPI_IRQ_BASE);
}
