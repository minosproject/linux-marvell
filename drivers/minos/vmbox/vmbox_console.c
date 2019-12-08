/*
 * Copyright (C) 2019 Min Le (lemin9538@gmail.com)
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

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/serial_core.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "../../tty/hvc/hvc_console.h"

#include <asm/io.h>
#include "vmbox_bus.h"
#include "../minos.h"

#define VMBOX_HVC_COOLIE	0xeeffdd00
#define VMBOX_HVC_NR		8

#define VMBOX_HVC_STAT_CLOSED	0x0
#define VMBOX_HVC_STAT_OPENED	0x1

#define VMBOX_HVC_EVENT_HANGUP	(VMBOX_DEV_EVENT_USER_BASE + 0)
#define VMBOX_HVC_EVENT_RX	(VMBOX_DEV_EVENT_USER_BASE + 1)
#define VMBOX_HVC_EVENT_TX_FULL	(VMBOX_DEV_EVENT_USER_BASE + 2)

static struct vmbox_console *hvc_consoles[VMBOX_HVC_NR];
static DEFINE_SPINLOCK(vmbox_console_lock);
static int hvc_index;

/*
 * at least 8K size for the transfer buffer, and
 * in or out buffer size need 2^ align
 */
struct hvc_ring {
	char in[2048];
	char out[4096];
	volatile uint32_t in_read, in_write;
	volatile uint32_t out_read, out_write;
};

struct vmbox_console {
	int id;
	struct vmbox_device *vdev;
	struct hvc_struct *hvc;
	int vetrmno;
	int backend;
	int otherside_state;
	struct hvc_ring *ring;
};

static inline struct
vmbox_console *vtermno_to_vmbox_console(uint32_t vtermno)
{
	if ((vtermno & 0xff) >= VMBOX_HVC_NR)
		return NULL;

	return hvc_consoles[vtermno & 0xff];
}

#define VMBOX_CONSOLE_IDX(idx, size)	(idx & (size - 1))

static int vmbox_hvc_read_console(uint32_t vtermno, char *buf, int count)
{
	struct vmbox_console *vc = vtermno_to_vmbox_console(vtermno);
	struct hvc_ring *ring = vc->ring;
	uint32_t ridx, widx, size, recv = 0;
	char *buffer;

	if (!vc->backend) {
		ridx = ring->out_read;
		widx = ring->out_write;
		buffer = ring->out;
		size = sizeof(ring->out);
	} else {
		ridx = ring->in_read;
		widx = ring->in_write;
		buffer = ring->in;
		size = sizeof(ring->in);
	}

	mb();
	BUG_ON((widx - ridx) > size);

	/* index overflow ? */
	while ((ridx != widx) && (recv < count))
		buf[recv++] = buffer[VMBOX_CONSOLE_IDX(ridx++, size)];

	mb();
	if (!vc->backend)
		ring->out_read = ridx;
	else
		ring->in_read = ridx;

	if (recv && vc->vdev)
		vmbox_device_ipc_event(vc->vdev, VMBOX_HVC_EVENT_RX);

	return recv;
}

static int vmbox_hvc_write_console(uint32_t vtermno, const char *buf, int count)
{
	struct vmbox_console *vc = vtermno_to_vmbox_console(vtermno);
	struct hvc_ring *ring = vc->ring;
	uint32_t ridx, widx, size, send = 0;
	char *buffer;
	int len = count;

	if (vc->backend) {
		buffer = ring->out;
		size = sizeof(ring->out);
	} else {
		buffer = ring->in;
		size = sizeof(ring->in);
	}

	while (count) {
		if (vc->backend) {
			ridx = ring->out_read;
			widx = ring->out_write;
		} else {
			ridx = ring->in_read;
			widx = ring->in_write;
		}
		mb();

		/* in case overflow alway happend in frontend side */
		if (((widx - ridx) == size) && vc->backend && !vc->otherside_state) {
			ridx += count;
			ring->out_read = ridx;
			wmb();
		}

		while ((send < count) && (widx - ridx) < size)
			buffer[VMBOX_CONSOLE_IDX(widx++, size)] = buf[send++];

		wmb();
		if (vc->backend)
			ring->out_write = widx;
		else
			ring->in_write = widx;

		count -= send;

		if (send && vc->vdev)
			vmbox_device_vring_event(vc->vdev);

		/*
		 * buffer is full but there still some data needed to transfer
		 * need wait the otherside finish read the data, when other side
		 * read the data, it will send a RX message to this VM, then it
		 * can be waked up
		 */
		if (count && (!vc->otherside_state)) {
			hvc_sched_out();
		} else if (count && vc->otherside_state && vc->vdev) {
			vmbox_device_ipc_event(vc->vdev,
					VMBOX_HVC_EVENT_TX_FULL);
		}
	}

	return len;
}

static int vmbox_hvc_notifier_add(struct hvc_struct *hp, int irq)
{
	int ret;
	struct vmbox_console *vc = vtermno_to_vmbox_console(hp->vtermno);

	if (!vc)
		return -ENOENT;

	if (!vc->vdev)
		return 0;

	ret = notifier_add_irq(hp, irq);
	if (ret)
		return ret;

	/* indicate the other side that I have opened */
	vmbox_device_ipc_event(vc->vdev, VMBOX_DEV_EVENT_OPENED);

	return 0;
}

static void vmbox_hvc_notifier_del(struct hvc_struct *hp, int irq)
{
	struct vmbox_console *vc = vtermno_to_vmbox_console(hp->vtermno);

	if (!vc || !vc->vdev)
		return;

	notifier_del_irq(hp, irq);

	/* indicate the other side that I have closed */
	if (vc->vdev)
		vmbox_device_ipc_event(vc->vdev, VMBOX_DEV_EVENT_CLOSED);
}

static void vmbox_hvc_notifier_hangup(struct hvc_struct *hp, int irq)
{
	struct vmbox_console *vc = vtermno_to_vmbox_console(hp->vtermno);

	if (!vc || !vc->vdev)
		return;

	notifier_del_irq(hp, irq);

	/* indicate the other side that I have hangup */
	vmbox_device_ipc_event(vc->vdev, VMBOX_HVC_EVENT_HANGUP);
}

static const struct hv_ops vmbox_hvc_ops = {
	.get_chars = vmbox_hvc_read_console,
	.put_chars = vmbox_hvc_write_console,
	.notifier_add = vmbox_hvc_notifier_add,
	.notifier_del = vmbox_hvc_notifier_del,
	.notifier_hangup = vmbox_hvc_notifier_hangup,
};

static int vmbox_hvc_vring_init(struct vmbox_console *vc)
{
	void *base;
	struct vmbox_device *vdev = vc->vdev;

	if (vc->ring) {
		pr_info("vmbox console already init at console init\n");
		return 0;
	}

	/*
	 * rxbuf - at least 2048
	 */
	if (vdev->vring_mem_size < 8192)
		return -ENOSPC;

	base = vmbox_device_remap(vdev);
	if (!base)
		return -ENOMEM;

	vc->ring = (struct hvc_ring *)base;
	return 0;
}

static int vm0_read_console(uint32_t vtermno, char *buf, int count)
{
	return 0;
}

static int vm0_write_console(uint32_t vtermno, const char *buf, int count)
{
	return count;
}

static const struct hv_ops vm0_hvc_ops = {
	.get_chars = vm0_read_console,
	.put_chars = vm0_write_console,
};

static int vmbox_hvc_probe(struct vmbox_device *vdev)
{
	struct hvc_struct *hp;
	struct vmbox_console *vc;
	static int need_init = 1;

	pr_info("do vmbox hvc probe\n");

	if ((get_vmid() == 0) && need_init) {
		pr_info("register a fake hvc for vm0\n");
		need_init = 0;
		hvc_alloc(VMBOX_HVC_COOLIE + 0xff, 0, &vm0_hvc_ops, 16);
		hvc_index++;
	}

	/*
	 * if the hvc in this VM is a forentend this hvc console
	 * will register at console_init stage, so do not realloc
	 * this hvc console at this stage
	 */
	if (hvc_consoles[hvc_index])
		vc = hvc_consoles[hvc_index];
	else {
		vc = kzalloc(sizeof(*vc), GFP_KERNEL);
		if (!vc)
			return -ENOMEM;
		vc->backend = vmbox_device_is_backend(vdev);
	}

	vmbox_set_drvdata(vdev, vc);
	vc->vdev = vdev;
	vc->vetrmno = VMBOX_HVC_COOLIE + hvc_index;
	spin_lock(&vmbox_console_lock);
	vc->id = hvc_index++;
	hvc_consoles[vc->id] = vc;
	spin_unlock(&vmbox_console_lock);

	/* init the vmbox device and the console */
	vmbox_device_init(vdev, VMBOX_F_NO_VIRTQ);

	if (vmbox_hvc_vring_init(vc)) {
		kfree(vc);
		return -ENOMEM;
	}

	hp = hvc_alloc(vc->vetrmno, vdev->vring_irq,
			&vmbox_hvc_ops, 256);
	if (IS_ERR(hp)) {
		kfree(vc);
		return PTR_ERR(hp);
	}

	vmbox_device_online(vdev);

	return 0;
}

static void vmbox_hvc_flush_data(struct vmbox_console *vc, int size)
{
	struct hvc_ring *ring = vc->ring;

	if (vc->vdev && vmbox_device_is_backend(vc->vdev))
		ring->out_read += size;
	else
		ring->in_read += size;
	mb();
}

static void vmbox_hvc_remove(struct vmbox_device *vdev)
{
	struct vmbox_console *vc = vmbox_get_drvdata(vdev);

	vmbox_device_offline(vdev);
	hvc_consoles[vc->id] = NULL;
	vmbox_device_unmap(vdev);
	kfree(vc);
}

static int vmbox_hvc_evt_handler(struct vmbox_device *vdev,
		uint32_t event)
{
	struct vmbox_console *vc = vmbox_get_drvdata(vdev);

	switch (event) {
	case VMBOX_DEV_EVENT_OPENED:
		vc->otherside_state = VMBOX_HVC_STAT_OPENED;
		break;
	case VMBOX_DEV_EVENT_CLOSED:
		vc->otherside_state = VMBOX_HVC_STAT_CLOSED;
		break;
	case VMBOX_HVC_EVENT_HANGUP:
		vc->otherside_state = VMBOX_HVC_STAT_CLOSED;
		break;
	case VMBOX_HVC_EVENT_RX:
		break;
	case VMBOX_HVC_EVENT_TX_FULL:
		vmbox_hvc_flush_data(vc, 16);
		break;
	default:
		break;
	}

	return 0;
}

static struct vmbox_device_id vmbox_hvc_ids[] = {
	{0x3420, VMBOX_ANY_VENDOR_ID},
	{0x3421, VMBOX_ANY_VENDOR_ID},
	{}
};

static struct vmbox_driver vmbox_console_drv = {
	.id_table = vmbox_hvc_ids,
	.probe = vmbox_hvc_probe,
	.remove = vmbox_hvc_remove,
	.otherside_evt_handler = vmbox_hvc_evt_handler,
	.driver = {
		.name = "vmbox-console",
	},
};

static int __init vmbox_console_init(void)
{
	return vmbox_register_driver(&vmbox_console_drv);
}

static void __exit vmbox_console_exit(void)
{
	vmbox_unregister_driver(&vmbox_console_drv);
}

module_init(vmbox_console_init);
module_exit(vmbox_console_exit);
MODULE_LICENSE("GPL");

static int __init vmbox_hvc_console_init(void)
{
	struct device_node *node;
	struct resource reg;
	struct vmbox_console *vc;
	struct hvc_ring *console_ring;

	pr_info("vmbox hvc console init for backend\n");

	/*
	 * to detected whether there is a vmbox hvc froent
	 * device, if yes, register it, the hvc index will start
	 * at 0, otherwise, it means this vm is HVM, the hvc console
	 * index whill start at 1, since systemd will automaticlly
	 * open hvc0
	 */
	node = of_find_compatible_node(NULL, NULL, "minos,hvc-be");
	if (!node) {
		pr_err("can not find the hvc console device\n");
		return -ENOENT;
	}

	if (of_address_to_resource(node, 0, &reg)) {
		pr_err("can not get hvc address\n");
		return -ENOMEM;
	}

	console_ring = ioremap_cache(reg.start, resource_size(&reg));
	if (!console_ring)
		return -ENOMEM;

	vc = kzalloc(sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;

	vc->backend = 1;
	vc->ring = console_ring;
	vc->vetrmno = VMBOX_HVC_COOLIE + 0;
	console_ring->out_read = 0;
	console_ring->in_write = 0;
	console_ring->in_read = 0;
	console_ring->out_write = 0;
	hvc_consoles[hvc_index] = vc;
	wmb();

	hvc_instantiate(VMBOX_HVC_COOLIE + 0, 0, &vmbox_hvc_ops);
	add_preferred_console("hvc", 0, NULL);

	return 0;
}
console_initcall(vmbox_hvc_console_init);
