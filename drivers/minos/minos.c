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
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <linux/minos.h>

#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>
#include <linux/mman.h>
#include <asm/pgtable-types.h>

#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>

struct vm_event {
	void *handler;
} __attribute__((__packed__));

#define	MINOS_VM_MAJOR		(278)
#define NETLINK_MVM		(29)

#define EVENT_PAGE_NR	((sizeof(struct vm_event) * MVM_MAX_EVENT) >> PAGE_SHIFT)

struct vm_event *vm_event_table;

static struct sock *mvm_sock;

static int create_vm_device(int vmid, struct vm_info *vm_info);

struct class *vm_class;
static LIST_HEAD(vm_list);
static DEFINE_MUTEX(vm_mutex);

#define dev_to_vm(_dev) \
	container_of(_dev, struct vm_device, device)

#define file_to_vm(_filp) \
	(struct vm_device *)(_filp->private_data)

#define VM_INFO_SHOW(_member, format)	\
	static ssize_t vm_ ## _member ## _show(struct device *dev, \
			struct device_attribute * attr, char *buf) \
	{ \
		struct vm_device *vm = dev_to_vm(dev); \
		struct vm_info *info = &vm->vm_info; \
		return sprintf(buf, format, info->_member); \
	}

VM_INFO_SHOW(mem_start, "0x%llx\n")
VM_INFO_SHOW(bit64, "%d\n")
VM_INFO_SHOW(mem_size, "0x%llx\n")
VM_INFO_SHOW(entry, "0x%llx\n")
VM_INFO_SHOW(setup_data, "0x%llx\n")
VM_INFO_SHOW(nr_vcpus, "%d\n")
VM_INFO_SHOW(name, "%s\n")
VM_INFO_SHOW(os_type, "%s\n")

static ssize_t
vm_vmid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vm_device *vm = dev_to_vm(dev);

	return sprintf(buf, "%d\n", vm->vmid);
}

static DEVICE_ATTR(vmid, 0444, vm_vmid_show, NULL);
static DEVICE_ATTR(mem_size, 0444, vm_mem_size_show, NULL);
static DEVICE_ATTR(nr_vcpus, 0444, vm_nr_vcpus_show, NULL);
static DEVICE_ATTR(mem_start, 0444, vm_mem_start_show, NULL);
static DEVICE_ATTR(entry, 0444, vm_entry_show, NULL);
static DEVICE_ATTR(setup_data, 0444, vm_setup_data_show, NULL);
static DEVICE_ATTR(name, 0444, vm_name_show, NULL);
static DEVICE_ATTR(os_type, 0444, vm_os_type_show, NULL);
static DEVICE_ATTR(bit64, 0444, vm_bit64_show, NULL);

static int mvm_open(struct inode *inode, struct file *file)
{
	int vmid = iminor(inode), err;
	struct vm_device *tmp, *vm = NULL;
	struct file_operations *new_fops = NULL;

	mutex_lock(&vm_mutex);

	list_for_each_entry(tmp, &vm_list, list) {
		if (vmid == tmp->vmid) {
			vm = tmp;
			new_fops = fops_get(vm->fops);
			break;
		}
	}

	if ((vm == NULL) || (!new_fops)) {
		pr_err("no such vm with vmid:%d\n", vmid);
		return -ENOENT;
	}

	file->private_data = vm;
	replace_fops(file, new_fops);
	err = 0;

	if (file->f_op->open)
		err = file->f_op->open(inode, file);

	mutex_unlock(&vm_mutex);
	return err;
}

struct file_operations mvm_fops = {
	.owner		= THIS_MODULE,
	.open		= mvm_open,
	.llseek		= noop_llseek,
};

static void vm_dev_free(struct device *dev)
{
	struct vm_device *vm = container_of(dev, struct vm_device, device);

	pr_info("release vm-%d\n", vm->vmid);
	kfree(vm);
}

static int vm_device_register(struct vm_device *vm)
{
	dev_t dev;
	int err = 0;

	INIT_LIST_HEAD(&vm->list);
	dev = MKDEV(MINOS_VM_MAJOR, vm->vmid);

	vm->device.class = vm_class;
	vm->device.devt = dev;
	vm->device.parent = NULL;
	vm->device.release = vm_dev_free;
	dev_set_name(&vm->device, "mvm%d", vm->vmid);
	device_initialize(&vm->device);

	err = device_add(&vm->device);
	if (err)
		return err;

	mutex_lock(&vm_mutex);
	list_add_tail(&vm->list, &vm_list);
	mutex_unlock(&vm_mutex);

	device_create_file(&vm->device, &dev_attr_vmid);
	device_create_file(&vm->device, &dev_attr_nr_vcpus);
	device_create_file(&vm->device, &dev_attr_mem_size);
	device_create_file(&vm->device, &dev_attr_mem_start);
	device_create_file(&vm->device, &dev_attr_entry);
	device_create_file(&vm->device, &dev_attr_setup_data);
	device_create_file(&vm->device, &dev_attr_name);
	device_create_file(&vm->device, &dev_attr_os_type);
	device_create_file(&vm->device, &dev_attr_bit64);

	return 0;
}

static int destroy_vm(int vmid)
{
	struct vm_device *vm = NULL;
	struct vm_device *tmp = NULL;

	if (vmid == 0)
		return -EINVAL;

	mutex_lock(&vm_mutex);
	list_for_each_entry(tmp, &vm_list, list) {
		if (tmp->vmid == vmid) {
			vm = tmp;
			break;
		}
	}
	mutex_unlock(&vm_mutex);

	if (vm == NULL)
		return -ENOENT;

	if (atomic_read(&vm->opened)) {
		pr_err("vm%d has been opened, release it first\n", vm->vmid);
		return -EBUSY;
	}

	mutex_lock(&vm_mutex);
	list_del(&vm->list);
	mutex_unlock(&vm_mutex);

	device_remove_file(&vm->device, &dev_attr_vmid);
	device_remove_file(&vm->device, &dev_attr_nr_vcpus);
	device_remove_file(&vm->device, &dev_attr_mem_size);
	device_remove_file(&vm->device, &dev_attr_mem_start);
	device_remove_file(&vm->device, &dev_attr_entry);
	device_remove_file(&vm->device, &dev_attr_setup_data);
	device_remove_file(&vm->device, &dev_attr_name);
	device_remove_file(&vm->device, &dev_attr_os_type);
	device_remove_file(&vm->device, &dev_attr_bit64);

	device_destroy(vm_class, MKDEV(MINOS_VM_MAJOR, vm->vmid));
	hvc_vm_destroy(vmid);

	return 0;
}

static int vm_release(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = file_to_vm(filp);

	if (!vm) {
		pr_err("vm has not been opend\n");
		return -ENOENT;
	}

	filp->private_data = NULL;
	atomic_cmpxchg(&vm->opened, 1, 0);

	return 0;
}

static int vm_open(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = (struct vm_device *)filp->private_data;

	if (atomic_cmpxchg(&vm->opened, 0, 1)) {
		pr_err("minos: vm%d has been opened\n", vm->vmid);
		return -EBUSY;
	}

	return 0;
}

static void send_vm_event(int pid, unsigned long arg)
{
	struct nlmsghdr *nlh;
	struct sk_buff *msg_skb;
	void *data;
	int ret;

	pr_debug("send 0x%lx to pid-%d\n", arg, pid);

	msg_skb = nlmsg_new(sizeof(void *), GFP_KERNEL);
	if (msg_skb)
		return;

	nlh = nlmsg_put(msg_skb, GFP_KERNEL, 0, 0, sizeof(void *), 0);
	if (!nlh)
		return;

	data = nlmsg_data(nlh);
	memcpy(data, (void *)&arg, sizeof(unsigned long));

	/*
	 * TBD - should get a spin lock here ?
	 */
	ret = nlmsg_unicast(mvm_sock, msg_skb, pid);
	if (ret)
		pr_err("send 0x%lx to pid-%d failed\n", arg, pid);
}

static irqreturn_t vm_event_handler(int irq, void *data)
{
	struct vm_event *event;
	struct vm_device *vm = (struct vm_device *)data;

	if (!vm)
		return IRQ_NONE;

	if ((irq < MVM_EVENT_ID_BASE) || (irq >= MVM_EVENT_ID_END))
		return IRQ_NONE;

	event = &vm_event_table[irq - MVM_EVENT_ID_BASE];
	if (!event) {
		pr_err("event-%d is not register for vm-%d\n", irq, vm->vmid);
		return IRQ_NONE;
	}

	send_vm_event(vm->pid, (unsigned long)event->handler);

	return IRQ_HANDLED;
}

static int register_vm_event(struct vm_device *vm, int pid, int irq, void *arg)
{
	int ret;
	struct vm_event *event;
	char buf[64];

	pr_info("register event %d 0x%p\n", irq, arg);
	if ((irq >= MVM_EVENT_ID_END) || (irq < MVM_EVENT_ID_BASE))
		return -EINVAL;

	event = &vm_event_table[irq - MVM_EVENT_ID_BASE];
	if (event->handler)
		pr_warn("event alrady register\n");

	vm->pid = pid;
	event->handler = arg;

	memset(buf, 0, 64);
	sprintf(buf, "vm%d-event%d", vm->vmid, irq);

	ret = request_threaded_irq(irq, NULL, vm_event_handler,
			IRQF_SHARED, buf, vm);
	if (ret)
		event->handler = NULL;

	return 0;
}

static long vm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct vm_device *vm = file_to_vm(filp);
	uint64_t kernel_arg[2];
	void *iomem;

	if (!vm)
		return -ENOENT;

	switch (cmd) {
	case IOCTL_RESTART_VM:
		break;
	case IOCTL_POWER_DOWN_VM:
		break;

	case IOCTL_POWER_UP_VM:
		hvc_vm_power_up(vm->vmid);
		break;

	case IOCTL_VM_MMAP:
		if (copy_from_user((void *)kernel_arg, (void *)arg,
				sizeof(uint64_t) * 2))
			return -EINVAL;
		hvc_vm_mmap(vm->vmid, kernel_arg[0], kernel_arg[1]);
		return 0;

	case IOCTL_VM_UNMAP:
		hvc_vm_unmap(vm->vmid);
		break;

	case IOCTL_REGISTER_MDEV:
		if (copy_from_user((void *)kernel_arg, (void *)arg,
				sizeof(uint64_t) * 2))
			return -EINVAL;

		return register_vm_event(vm, kernel_arg[0] >> 32,
				kernel_arg[0] & 0xffffffff,
				(void *)kernel_arg[1]);
	case IOCTL_SEND_VIRQ:
		hvc_send_virq(vm->vmid, (uint32_t)arg);
		break;

	case IOCTL_CREATE_VIRTIO_DEVICE:
		iomem = hvc_create_virtio_device(vm->vmid);
		if (copy_to_user((void *)arg, &iomem, sizeof(void *)))
			return -EIO;
		return 0;
	default:
		break;
	}

	return 0;
}

static unsigned long
mvm_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct vm_device *vm = file_to_vm(file);
	struct vm_unmapped_area_info info;

	if (len & (vm->guest_page_size - 1))
		return -EINVAL;
	if (len > TASK_SIZE)
		return -ENOMEM;

	if (flags & MAP_FIXED)
		return -EINVAL;

	if (addr) {
		addr = ALIGN(addr, vm->guest_page_size);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr &&
			(!vma || addr + len <= vma->vm_start))
			return addr;
	}

	/*
	 * alloc an pud aligned vma area to map 2m pmd
	 */
	info.flags = 0;
	info.length = len;
	info.low_limit = mm->mmap_base;
	info.high_limit = TASK_SIZE;
	info.align_mask = PAGE_MASK & ~PMD_MASK;
	info.align_offset = 0;

	return vm_unmapped_area(&info);
}

static pte_t *mvm_pmd_alloc(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;

	pgd = pgd_offset(mm, addr);
	if (*pgd) {
		return (pte_t *)pmd_offset((pud_t *)pgd, addr);
	} else
		return (pte_t *)pmd_alloc(mm, (pud_t *)pgd, addr);
}

static int mvm_vm_mmap(struct file *file, struct vm_area_struct *vma)
{
	pte_t *ptep;
	int count, i;
	pmd_t pmd;
	unsigned long offset, mmap_base, addr;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_device *vm = file_to_vm(file);
	struct vm_info *info = &vm->vm_info;
	unsigned long vma_size = vma->vm_end - vma->vm_start;

	/*
	 * now minos only support 2M block so using pmd
	 * mapping, the va_start must PUD size align
	 */
	if ((!vm) || (!info->mmap_base) || (!info->mem_size))
		return -ENOENT;

	if (vma->vm_start & (vm->guest_page_size - 1))
		return -EINVAL;

	if (vma_size & (vm->guest_page_size - 1))
		return -EINVAL;

	pr_debug("vm-%d map 0x%lx -> 0x%llx size:0x%lx\n",
			vm->vmid, vma->vm_start,
			info->mmap_base, vma_size);

	vma_size = vma_size >> PMD_SHIFT;
	mmap_base = info->mmap_base;
	vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_flags &= ~(VM_MAYWRITE);
	addr = vma->vm_start;

	flush_cache_range(vma, vma->vm_start, vma->vm_end);
	while (vma_size > 0) {
		ptep = mvm_pmd_alloc(mm, addr);
		if (!ptep)
			BUG_ON(!ptep);

		offset = pmd_index(addr);
		count = PTRS_PER_PMD - offset;
		count = count > vma_size ? vma_size : count;

		for (i = 0; i < count; i++) {
			pmd = pmd_mkhuge_normal(mmap_base);
			pmd = pmd_mkdirty(pmd);
			if (vma->vm_flags & VM_WRITE)
				pmd = pmd_mkwrite(pmd);
			set_pmd_at(mm, addr, ptep + i, pmd);
			mmap_base += PMD_SIZE;
			addr += PMD_SIZE;
		}

		vma_size -= count;
	}

	return 0;
}

struct file_operations vm_fops = {
	.open			= vm_open,
	.release		= vm_release,
	.unlocked_ioctl 	= vm_ioctl,
	.mmap			= mvm_vm_mmap,
	.get_unmapped_area	= mvm_get_unmapped_area,
	.owner			= THIS_MODULE,
};

static int vm0_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int vm0_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int create_new_vm(struct vm_info *info)
{
	int vmid;

	if (!info)
		return -EINVAL;

	vmid = hvc_vm_create(info);
	if (vmid <= 0) {
		pr_err("unable to create new vm\n");
		return vmid;
	}

	create_vm_device(vmid, info);

	return vmid;
}

static long vm0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret, vmid;
	struct vm_info vm_info;

	switch (cmd) {
	case IOCTL_CREATE_VM:
		memset(&vm_info, 0, sizeof(struct vm_info));
		ret = copy_from_user(&vm_info, (void *)arg, sizeof(struct vm_info));
		if (ret)
			return -EINVAL;

		vmid = create_new_vm(&vm_info);
		if (vmid <= 0)
			return vmid;

		ret = copy_to_user((void *)arg, (void *)&vm_info,
				sizeof(struct vm_info));
		if (ret)
			pr_err("copy vm info to user failed\n");

		return vmid;

	case IOCTL_DESTROY_VM:
		destroy_vm((int)arg);
		break;
	default:
		break;
	}

	return -EINVAL;
}

static int vm0_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long phy = vma->vm_pgoff << PAGE_SHIFT;

	vma->vm_pgoff = 0;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (vm_iomap_memory(vma, phy, size)) {
		pr_err("map 0x%lx -> 0x%lx size:0x%lx failed\n",
			vma->vm_start, phy, size);
		return -EAGAIN;
	}

	return 0;
}

static struct file_operations vm0_fops = {
	.open		= vm0_open,
	.release	= vm0_release,
	.unlocked_ioctl = vm0_ioctl,
	.mmap		= vm0_mmap,
	.owner		= THIS_MODULE,
};

static int create_vm_device(int vmid, struct vm_info *vm_info)
{
	int ret;
	struct vm_device *vm;

	vm = kzalloc(sizeof(struct vm_device), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	/* fix the guest_page_size to PMD_SIZE(2M on arm64) now */
	vm->vmid = vmid;
	vm->guest_page_size = PMD_SIZE;
	if (vm_info)
		memcpy(&vm->vm_info, vm_info, sizeof(struct vm_info));

	if (vmid == 0)
		vm->fops = &vm0_fops;
	else
		vm->fops = &vm_fops;

	ret = vm_device_register(vm);
	if (ret)
		goto out_free_vm;

	return 0;

out_free_vm:
	kfree(vm);
	return ret;
}

static char *vm_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "mvm/%s", dev_name(dev));
}

static void mvm_netlink_input(struct sk_buff *skb)
{

}

static int __init minos_init(void)
{
	int err;
	struct netlink_kernel_cfg cfg = {
		.input = mvm_netlink_input,
	};

	vm_class = class_create(THIS_MODULE, "mvm");
	err = PTR_ERR(vm_class);
	if (IS_ERR(vm_class))
		return err;

	err = register_chrdev(MINOS_VM_MAJOR, "mvm", &mvm_fops);
	if (err) {
		printk("unable to get major %d for mvm devices\n",
				MINOS_VM_MAJOR);
		goto destroy_class;
	}

	vm_class->devnode = vm_devnode;

	err = create_vm_device(0, NULL);
	if (err)
		goto unregister_chardev;

	vm_event_table = (struct vm_event *)
		__get_free_pages(GFP_KERNEL, EVENT_PAGE_NR);
	if (!vm_event_table)
		goto destroy_class;

	memset(vm_event_table, 0, EVENT_PAGE_NR * PAGE_SIZE);
	mvm_sock = netlink_kernel_create(&init_net, NETLINK_MVM, &cfg);
	if (!mvm_sock) {
		pr_err("create mvm netlink failed\n");
		goto free_table;
	}

	return 0;

free_table:
	kfree(vm_event_table);

unregister_chardev:
	unregister_chrdev(MINOS_VM_MAJOR, "mvm");

destroy_class:
	class_destroy(vm_class);

	return -1;
}

static void minos_exit(void)
{
	/* remove all vm which has created */
	class_destroy(vm_class);
	unregister_chrdev(MINOS_VM_MAJOR, "mvm");

	if (vm_event_table)
		kfree(vm_event_table);

	netlink_kernel_release(mvm_sock);
}

module_init(minos_init);
module_exit(minos_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Min Le lemin@gmail.com");
