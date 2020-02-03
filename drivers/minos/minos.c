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
#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>
#include <linux/mman.h>
#include <linux/platform_device.h>
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/eventfd.h>
#include <linux/interrupt.h>
#include <linux/of.h>

#ifdef CONFIG_ARM64
#include <asm/pgtable-types.h>
#else
#include <asm/mach/map.h>
#endif

#include "minos.h"

struct vm_event {
	struct eventfd_ctx *ctx;
	struct vm_device *vm;
} __attribute__((__packed__));

#define	MINOS_VM_MAJOR		(278)
#define NETLINK_MVM		(29)

#define EVENT_PAGE_NR	((sizeof(struct vm_event) * MVM_MAX_EVENT) >> PAGE_SHIFT)

struct vm_event *vm_event_table;

static int create_vm_device(int vmid, struct vmtag *vmtag);

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
		struct vmtag *info = &vm->vmtag; \
		return sprintf(buf, format, info->_member); \
	}

VM_INFO_SHOW(mem_base, "0x%llx\n")
VM_INFO_SHOW(flags, "0x%llx\n")
VM_INFO_SHOW(mem_size, "0x%llx\n")
VM_INFO_SHOW(entry, "0x%llx\n")
VM_INFO_SHOW(setup_data, "0x%llx\n")
VM_INFO_SHOW(nr_vcpu, "%d\n")
VM_INFO_SHOW(name, "%s\n")
VM_INFO_SHOW(os_type, "%s\n")

static ssize_t
vm_vmid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vm_device *vm = dev_to_vm(dev);

	return sprintf(buf, "%d\n", vm->vmid);
}

static ssize_t hv_log_level_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int level;

	sscanf(buf, "%d", &level);
	hvc_change_log_level(level);

	return count;
}

static ssize_t
hv_log_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", 0);
}

static DEVICE_ATTR(vmid, 0444, vm_vmid_show, NULL);
static DEVICE_ATTR(mem_size, 0444, vm_mem_size_show, NULL);
static DEVICE_ATTR(nr_vcpu, 0444, vm_nr_vcpu_show, NULL);
static DEVICE_ATTR(mem_base, 0444, vm_mem_base_show, NULL);
static DEVICE_ATTR(entry, 0444, vm_entry_show, NULL);
static DEVICE_ATTR(setup_data, 0444, vm_setup_data_show, NULL);
static DEVICE_ATTR(name, 0444, vm_name_show, NULL);
static DEVICE_ATTR(os_type, 0444, vm_os_type_show, NULL);
static DEVICE_ATTR(flags, 0444, vm_flags_show, NULL);

static DEVICE_ATTR(log_level, S_IWUSR | S_IRUGO,
		hv_log_level_show, hv_log_level_store);

static struct vm_device *vmid_to_vm(uint32_t vmid)
{
	struct vm_device *tmp;

	mutex_lock(&vm_mutex);
	list_for_each_entry(tmp, &vm_list, list) {
		if (vmid == tmp->vmid) {
			mutex_unlock(&vm_mutex);
			return tmp;
		}
	}
	mutex_unlock(&vm_mutex);

	return NULL;
}

static int mvm_open(struct inode *inode, struct file *file)
{
	struct vm_device *vm;
	int vmid = iminor(inode), err = 0;
	struct file_operations *new_fops = NULL;

	vm = vmid_to_vm(vmid);
	if (!vm) {
		pr_err("vm-%d has not been created\n", vmid);
		return -ENOENT;
	}

	new_fops = fops_get(vm->fops);
	if (!new_fops) {
		pr_err("no such vm with vmid:%d\n", vmid);
		return -ENOENT;
	}

	mutex_lock(&vm_mutex);
	replace_fops(file, new_fops);
	mutex_unlock(&vm_mutex);

	if (vmid != 0)
		file->private_data = vm;
	else
		file->private_data = NULL;

	if (file->f_op->open)
		err = file->f_op->open(inode, file);

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

	pr_info("release vm-%d memory\n", vm->vmid);
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

	if (vm->vmid != 0) {
		device_create_file(&vm->device, &dev_attr_vmid);
		device_create_file(&vm->device, &dev_attr_nr_vcpu);
		device_create_file(&vm->device, &dev_attr_mem_size);
		device_create_file(&vm->device, &dev_attr_mem_base);
		device_create_file(&vm->device, &dev_attr_entry);
		device_create_file(&vm->device, &dev_attr_setup_data);
		device_create_file(&vm->device, &dev_attr_name);
		device_create_file(&vm->device, &dev_attr_os_type);
		device_create_file(&vm->device, &dev_attr_flags);
	} else {
		device_create_file(&vm->device, &dev_attr_log_level);
	}

	return 0;
}

static int vm_release(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = file_to_vm(filp);

	pr_info("release vm-%d\n", vm->vmid);

	if ((vm == NULL) && (vm->owner != current)) {
		pr_err("can not destroy vm not the owner\n");
		return -EPERM;
	}

	vm->owner = NULL;
	filp->private_data = NULL;

	mutex_lock(&vm_mutex);
	list_del(&vm->list);
	mutex_unlock(&vm_mutex);

	if (vm->vmid != 0) {
		device_remove_file(&vm->device, &dev_attr_vmid);
		device_remove_file(&vm->device, &dev_attr_nr_vcpu);
		device_remove_file(&vm->device, &dev_attr_mem_size);
		device_remove_file(&vm->device, &dev_attr_mem_base);
		device_remove_file(&vm->device, &dev_attr_entry);
		device_remove_file(&vm->device, &dev_attr_setup_data);
		device_remove_file(&vm->device, &dev_attr_name);
		device_remove_file(&vm->device, &dev_attr_os_type);
		device_remove_file(&vm->device, &dev_attr_flags);
	} else {
		device_remove_file(&vm->device, &dev_attr_log_level);
	}

	device_destroy(vm_class, MKDEV(MINOS_VM_MAJOR, vm->vmid));
	hvc_vm_destroy(vm->vmid);

	return 0;
}

static int vm_open(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = (struct vm_device *)filp->private_data;

	if (vm->owner != current) {
		pr_err("can not open this VM not the owner\n");
		return -EPERM;
	}

	if (atomic_cmpxchg(&vm->opened, 0, 1)) {
		pr_err("minos: vm%d has been opened\n", vm->vmid);
		return -EBUSY;
	}

	return 0;
}

static irqreturn_t vm_event_handler(int irq, void *data)
{
	struct vm_event *event;
	int hwirq = (int)((unsigned long)data);

	if (!hwirq)
		return IRQ_NONE;

	if ((hwirq < MVM_EVENT_ID_BASE) || (hwirq >= MVM_EVENT_ID_END))
		return IRQ_NONE;

	event = &vm_event_table[hwirq - MVM_EVENT_ID_BASE];
	if (!event) {
		pr_err("event-%d is not register\n", irq);
		return IRQ_NONE;
	}

	if (!event->vm || !event->ctx)
		return IRQ_NONE;

	eventfd_signal(event->ctx, 1);

	return IRQ_HANDLED;
}

static int unregister_vm_event(struct vm_device *vm, int irq)
{
	int virq;
	struct vm_event *event;

	pr_info("unregister irq-%d\n", irq);
	if ((irq >= MVM_EVENT_ID_END) || (irq < MVM_EVENT_ID_BASE))
		return -EINVAL;

	event = &vm_event_table[irq - MVM_EVENT_ID_BASE];
	if (!event->ctx) {
		pr_warn("event %d not register\n", irq);
		return -ENODEV;
	}

	virq = get_dynamic_virq(irq);
	if (!irq) {
		pr_err("can not get the irq of device\n");
		return -ENOENT;
	}

	free_irq(virq, (void *)((unsigned long)irq));
	event->ctx = NULL;
	event->vm = NULL;

	return 0;
}

static int register_vm_event(struct vm_device *vm, int eventfd, int irq)
{
	int ret;
	int virq;
	struct eventfd_ctx *ctx;
	struct vm_event *event;
	struct file *eventfp;
	char *name;

	pr_info("register event-%d irq-%d\n", eventfd, irq);
	if ((irq >= MVM_EVENT_ID_END) || (irq < MVM_EVENT_ID_BASE))
		return -EINVAL;

	event = &vm_event_table[irq - MVM_EVENT_ID_BASE];
	if (event->ctx)
		pr_warn("event alrady register\n");

	eventfp = eventfd_fget(eventfd);
	if (!eventfp) {
		pr_err("can not the file of the eventfd\n");
		return -ENOENT;
	}

	ctx = eventfd_ctx_fileget(eventfp);
	if (!ctx) {
		pr_err("can not get the eventfd ctx\n");
		return -ENOENT;
	}

	event->ctx = ctx;
	event->vm = vm;

	virq = get_dynamic_virq(irq);
	if (!irq) {
		pr_err("can not get the irq of device\n");
		return -ENOENT;
	}

	name = kmalloc(32, GFP_KERNEL);
	if (!name) {
		event->ctx = NULL;
		return -ENOMEM;
	}

	memset(name, 0, 32);
	sprintf(name, "vm%d-irq%d", vm->vmid, irq);

	ret = request_irq(virq, vm_event_handler, 0,
			name, (void *)((unsigned long)irq));
	if (ret) {
		pr_err("request event irq failed %d %d\n", irq, ret);
		event->ctx = NULL;
		return ret;
	}

	return 0;
}

static inline int ioctl_vm_mmap(struct vm_device *vm, uint64_t __user *p)
{
	int ret;
	uint64_t mem_start, mem_size;

	ret = get_user(mem_start, p);
	if (ret)
		return ret;

	ret = get_user(mem_size, (p + 1));
	if (ret)
		return ret;

	ret = hvc_vm_mmap(vm->vmid, mem_start, mem_size, &vm->vm0_mmap_base);
	if (ret) {
		pr_err("map vm memory to vm0 space failed\n");
		return -ENOMEM;
	}

	return 0;
}

static int inline ioctl_register_vcpu(struct vm_device *vm, uint32_t __user *p)
{
	int ret;
	uint32_t fd, virq;

	ret = get_user(fd, p);
	if (ret)
		return ret;

	ret = get_user(virq, p + 1);
	if (ret)
		return ret;

	return register_vm_event(vm, fd, virq);
}

static inline int ioctl_create_vmcs(struct vm_device *vm, uint64_t __user *p)
{
	uint64_t iomem = hvc_create_vmcs(vm->vmid);
	if (!iomem)
		return -ENOMEM;

	return put_user(iomem, p);
}

static inline int ioctl_request_virq(struct vm_device *vm, int __user *p)
{
	int base, size, ret;

	ret = get_user(base, p);
	if (ret)
		return ret;

	ret = get_user(size, p + 1);
	if (ret)
		return ret;

	return hvc_request_virq(vm->vmid, base, size);
}

static int ioctl_virtio_mmio_init(struct vm_device *vm, uint64_t __user *p)
{
	int ret;
	uint64_t gbase, size;
	unsigned long hbase;

	ret = get_user(gbase, p);
	if (ret)
		return ret;

	ret = get_user(size, p + 1);
	if (ret)
		return ret;

	ret = hvc_virtio_mmio_init(vm->vmid, gbase, size, &hbase);
	if (ret) {
		pr_err("hvc mmio init failed %d\n", ret);
		return ret;
	}

	return put_user(hbase, p);
}

static long vm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	void __user *p = (void __user *)arg;
	struct vm_device *vm = file_to_vm(filp);

	if (!vm || (task_tgid_vnr(current) != task_tgid_vnr(vm->owner))) {
		pr_err("can not call ioctl not the owner 0x%x\n", cmd);
		return -ENOENT;
	}

	switch (cmd) {
	case IOCTL_RESTART_VM:
		ret = hvc_vm_reset(vm->vmid);
		break;
	case IOCTL_POWER_DOWN_VM:
		ret = hvc_vm_power_down(vm->vmid);
		break;
	case IOCTL_POWER_UP_VM:
		ret = hvc_vm_power_up(vm->vmid);
		break;
	case IOCTL_VM_MMAP:
		ret = ioctl_vm_mmap(vm, p);
		break;
	case IOCTL_UNREGISTER_VCPU:
		ret = unregister_vm_event(vm, (int)arg);
		break;
	case IOCTL_REGISTER_VCPU:
		ret = ioctl_register_vcpu(vm, p);
		break;
	case IOCTL_SEND_VIRQ:
		ret = hvc_send_virq(vm->vmid, (uint32_t)arg);
		break;
	case IOCTL_CREATE_VMCS:
		ret = ioctl_create_vmcs(vm, p);
		break;
	case IOCTL_CREATE_VMCS_IRQ:
		ret = hvc_create_vmcs_irq(vm->vmid, (int)arg);
		break;
	case IOCTL_REQUEST_VIRQ:
		ret = ioctl_request_virq(vm, p);
		break;
	case IOCTL_VIRTIO_MMIO_INIT:
		ret = ioctl_virtio_mmio_init(vm, p);
		break;
	case IOCTL_CREATE_VM_RESOURCE:
		ret = hvc_create_vm_resource(vm->vmid);
		break;
	default:
		ret = -ENOENT;
		pr_err("unsupported ioctl cmd\n");
		break;
	}

	return ret;
}

static unsigned long
mvm_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	unsigned long m_addr;
	struct vm_device *vm = file_to_vm(file);
	struct vm_unmapped_area_info info;

	if (len > TASK_SIZE)
		return -ENOMEM;

	if (flags & MAP_FIXED)
		return -EINVAL;

	/*
	 * alloc an PMD aligned vma area to map VM normal
	 * memory
	 */
	if (len & (vm->guest_page_size - 1))
		return -EINVAL;

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = TASK_UNMAPPED_BASE;
	info.high_limit = TASK_SIZE;
	info.align_mask = ~PMD_MASK;
	info.align_offset = 0;

	m_addr = vm_unmapped_area(&info);
	if (m_addr == -ENOMEM)
		pr_err("no memory for mmap\n");

	return m_addr;
}

#if defined(CONFIG_ARM64) || defined(CONFIG_ARM_LPAE)
static pmd_t *mvm_pmd_alloc(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;

	pgd = pgd_offset(mm, addr);
	if (pgd_val(*pgd)) {
		return (pmd_t *)pmd_offset((pud_t *)pgd, addr);
	} else
		return (pmd_t *)pmd_alloc(mm, (pud_t *)pgd, addr);
}
#endif

#if defined(CONFIG_ARM) && !defined(CONFIG_ARM_LPAE)
#define pmd_table(pmd) \
	((pmd_val(pmd) & PMD_TYPE_MASK) == PMD_TYPE_TABLE)
#endif

#ifdef CONFIG_ARM64
static void inline flush_pmd_entry(pmd_t *pmd) {}
#endif

int mvm_zap_pmd_range(struct vm_area_struct *vma, pmd_t *pmd)
{
	if ((pmd_val(*pmd) && !pmd_table(*pmd)) && (vma->vm_flags & VM_PFNMAP)) {
		/*
		 * if the VM_PFNMAP is set, indicate that the huge page is
		 * directly mapped to a physical address, just clear the pmd entry
		 * TBD
		 */
		memset(pmd, 0, sizeof(pmd_t));
		flush_pmd_entry(pmd);

		return 0;
	}

	return 1;
}

#ifdef CONFIG_ARM64
static int mvm_vm_mmap(struct file *file, struct vm_area_struct *vma)
{
	pmd_t *ptep;
	int i, count;
	pmd_t pmd;
	unsigned long offset, mmap_base, addr;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_device *vm = file_to_vm(file);
	struct vmtag *info = &vm->vmtag;
	unsigned long vma_size = vma->vm_end - vma->vm_start;

	/*
	 * now minos only support 1M Section for aarch32 so using
	 * pmd mapping, the va_start must PUD size align
	 */
	if ((!vm) || (vm->owner != current))
		return -EPERM;

	if (!info->mem_size)
		return -ENOENT;

	if (vma->vm_start & (vm->guest_page_size - 1))
		return -EINVAL;

	if (vma_size & (vm->guest_page_size - 1))
		return -EINVAL;

	pr_info("vm-%d map 0x%lx -> 0x%lx size:0x%lx\n",
			vm->vmid, vma->vm_start,
			vm->vm0_mmap_base, vma_size);

	vma_size = vma_size >> PMD_SHIFT;
	mmap_base = vm->vm0_mmap_base;
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
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
#elif !defined(CONFIG_ARM64) && !defined(CONFIG_ARM_LPAE)
static int mvm_vm_mmap(struct file *file, struct vm_area_struct *vma)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	unsigned long mmap_base;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_device *vm = file_to_vm(file);
	struct vmtag *info = &vm->vmtag;
	unsigned long addr = vma->vm_start, end = vma->vm_end;
	unsigned long vma_size = vma->vm_end - vma->vm_start;

	if ((!vm) || (vm->owner != current))
		return -EPERM;

	if (!info->mem_size)
		return -ENOENT;

	if (vma->vm_start & (vm->guest_page_size -1))
		return -EINVAL;

	if (vma_size & (vm->guest_page_size - 1))
		return -EINVAL;

	pr_info("vm-%d map 0x%lx -> 0x%lx size:0x%lx\n",
			vm->vmid, vma->vm_start,
			vm->vm0_mmap_base, vma_size);
	/*
	 * for arm32 if LPAE is not enabled kernel will 2 levels
	 * page table, if mapped as section each entry will map
	 * 1M memory section. currently non LAPE has not been tested
	 * there may some issue TBF
	 */
	vma->vm_flags |= 0x80000000 |
		VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_flags &= ~VM_MAYWRITE;
	mmap_base = vm->vm0_mmap_base;

	pgd = pgd_offset(mm, addr);
	pud = pud_offset(pgd, addr);
	pmd = pmd_offset(pud, addr);

	do {
		pmd[0] = __pmd(mmap_base | PMD_TYPE_SECT | PMD_SECT_S |
				PMD_SECT_AP_WRITE | PMD_SECT_AP_READ |
				PMD_SECT_PXN | PMD_SECT_XN | PMD_SECT_WBWA);
		mmap_base += SZ_1M;
		pmd[1] = __pmd(mmap_base | PMD_TYPE_SECT | PMD_SECT_S |
				PMD_SECT_AP_WRITE | PMD_SECT_AP_READ |
				PMD_SECT_PXN | PMD_SECT_XN | PMD_SECT_WBWA);
		mmap_base += SZ_1M;
		flush_pmd_entry(pmd);

		addr += PMD_SIZE;
		pmd += 2;
	} while (addr < end);

	flush_cache_vmap(vma->vm_start, vma->vm_end);

	return 0;
}
#else
static int mvm_vm_mmap(struct file *file, struct vm_area_struct *vma)
{
	pte_t *ptep;
	int i, count;
	pmd_t pmd;
	unsigned long offset, mmap_base, addr;
	struct mm_struct *mm = vma->vm_mm;
	struct vm_device *vm = file_to_vm(file);
	struct vmtag *info = &vm->vmtag;
	unsigned long vma_size = vma->vm_end - vma->vm_start;

	/*
	 * now minos only support 1M Section for aarch32 so using
	 * pmd mapping, the va_start must PUD size align
	 */
	if ((!vm) || (vm->owner != current))
		return -EPERM;

	if (!info->mem_size)
		return -ENOENT;

	if (vma->vm_start & (vm->guest_page_size - 1))
		return -EINVAL;

	if (vma_size & (vm->guest_page_size - 1))
		return -EINVAL;

	pr_info("vm-%d map 0x%lx -> 0x%lx size:0x%lx\n",
			vm->vmid, vma->vm_start,
			vm->vm0_mmap_base, vma_size);

	vma_size = vma_size >> PMD_SHIFT;
	mmap_base = vm->vm0_mmap_base;
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
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
			pmd = mmap_base | PMD_TYPE_SECT | PMD_SECT_AP_WRITE |
				L_PMD_SECT_DIRTY | L_PMD_SECT_VALID |
				PMD_SECT_USER | PMD_SECT_AF | PMD_SECT_S |
				PMD_SECT_PXN | PMD_SECT_XN | PMD_SECT_WBWA;
			*ptep = pmd;
			flush_pmd_entry(ptep);
			mmap_base += PMD_SIZE;
			addr += PMD_SIZE;
			ptep++;
		}

		vma_size -= count;
	}

	flush_cache_vmap(vma->vm_start, vma->vm_end);

	return 0;
}
#endif

#ifdef CONFIG_COMPAT
static long vm0_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return vm_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

struct file_operations vm_fops = {
	.open			= vm_open,
	.release		= vm_release,
	.unlocked_ioctl 	= vm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= vm_compat_ioctl,
#endif
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

static int create_new_vm(struct vmtag *info)
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

static int ioctl_create_vm(struct file *filp, void __user *p)
{
	int vmid;
	struct vmtag vmtag;

	vmid = copy_from_user(&vmtag, p, sizeof(struct vmtag));
	if (vmid)
		return -EACCES;

	vmid = create_new_vm(&vmtag);
	if (vmid <= 0) {
		pr_err("unable to create new guest VM\n");
		return vmid;
	}

	filp->private_data = vmid_to_vm(vmid);
	if (copy_to_user(p, &vmtag, sizeof(struct vmtag)))
			pr_err("copy vm info to user failed\n");

	return vmid;
}

static long vm0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;

	switch (cmd) {
	case IOCTL_CREATE_VM:
		ret = ioctl_create_vm(filp, (void __user *)arg);
		break;
	default:
		ret = -ENOENT;
		pr_err("unsupport vm0 ioctl cmd\n");
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long vm0_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return vm0_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int vm0_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vm_device *vm = file_to_vm(file);
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long phy = vma->vm_pgoff << PAGE_SHIFT;

	if (!vm || (vm->owner != current)) {
		pr_err("VM is not opend by his owner\n");
		return -EPERM;
	}

	vma->vm_pgoff = 0;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (vm_iomap_memory(vma, phy, size)) {
		pr_err("map 0x%lx -> 0x%lx size:0x%zx failed\n",
			vma->vm_start, phy, size);
		return -EAGAIN;
	}

	return 0;
}

static struct file_operations vm0_fops = {
	.open		= vm0_open,
	.release	= vm0_release,
	.unlocked_ioctl = vm0_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= vm0_compat_ioctl,
#endif
	.mmap		= vm0_mmap,
	.owner		= THIS_MODULE,
};

static int create_vm_device(int vmid, struct vmtag *vmtag)
{
	int ret;
	struct vm_device *vm;

	vm = kzalloc(sizeof(struct vm_device), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	/*
	 * this VM belongs to this task, other process
	 * can not open this task
	 */
	vm->owner = current;
	vm->vmid = vmid;
	vm->guest_page_size = PMD_SIZE;

	if (vmtag)
		memcpy(&vm->vmtag, vmtag, sizeof(struct vmtag));

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

static int minos_hv_probe(struct platform_device *pdev)
{
	int err;

	pr_info("Minos Hyperviosr Driver Init ...\n");

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
	pr_info("Minos Hyperviosr Driver Init Done\n");

	return 0;

unregister_chardev:
	unregister_chrdev(MINOS_VM_MAJOR, "mvm");

destroy_class:
	class_destroy(vm_class);

	return -1;
}

static int minos_hv_remove(struct platform_device *pdev)
{
	/* remove all vm which has created */
	class_destroy(vm_class);
	unregister_chrdev(MINOS_VM_MAJOR, "mvm");

	if (vm_event_table)
		kfree(vm_event_table);

	return 0;
}

static struct of_device_id minos_hv_match[] = {
	{.compatible = "minos,hypervisor", },
	{},
}
MODULE_DEVICE_TABLE(of, minos_hv_match);

static struct platform_driver minos_hv_driver = {
	.probe	= minos_hv_probe,
	.remove = minos_hv_remove,
	.driver = {
		.name = "minos-hypervisor",
		.of_match_table = minos_hv_match,
	},
};

static int minos_init(void)
{
	return platform_driver_register(&minos_hv_driver);
}

static void minos_exit(void)
{
	platform_driver_unregister(&minos_hv_driver);
}

module_init(minos_init);
module_exit(minos_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Min Le lemin@gmail.com");
