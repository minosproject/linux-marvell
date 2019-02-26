#ifndef __LINUX_MINOS_H__
#define __LINUX_MINOS_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/arm-smccc.h>

#include "minos_hypercall.h"
#include "minos_ioctl.h"

#define MINOS_VM_MAX			(64)
#define VM_NAME_SIZE	32
#define VM_TYPE_SIZE	16

struct vmtag {
	uint32_t vmid;
	char name[VM_NAME_SIZE];
	char os_type[VM_TYPE_SIZE];
	int32_t nr_vcpu;
	unsigned long mem_base;
	unsigned long mem_size;
	void *entry;
	void *setup_data;
	unsigned long flags;
	uint32_t vcpu_affinity[8];
	uint64_t mmap_base;
};

struct vm_device {
	int vmid;
	atomic_t opened;
	phys_addr_t pmem_map;
	unsigned long map_size;
	unsigned long guest_page_size;
	struct list_head list;
	struct vmtag vmtag;
	struct device *parent;
	struct device device;
	struct file_operations *fops;
};

#define MVM_EVENT_ID_BASE	(32)
#define MVM_MAX_EVENT		(512)
#define MVM_EVENT_ID_END	(MVM_EVENT_ID_BASE + MVM_MAX_EVENT)

static inline unsigned long __minos_hvc(uint32_t id, unsigned long a0,
       unsigned long a1, unsigned long a2, unsigned long a3,
       unsigned long a4, unsigned long a5)
{
   struct arm_smccc_res res;

   arm_smccc_hvc(id, a0, a1, a2, a3, a4, a5, 0, &res);
   return res.a0;
}

extern void minos_hvc_result1(void *x1);
extern void minos_hvc_result2(void *x1, void *x2);

#define minos_hvc(id, a, b, c, d, e, f) \
	__minos_hvc(id, (unsigned long)(a), (unsigned long)(b), \
		    (unsigned long)(c), (unsigned long)(d), \
		    (unsigned long)(e), (unsigned long)(f))

#define minos_hvc0(id) 				minos_hvc(id, 0, 0, 0, 0, 0, 0)
#define minos_hvc1(id, a)			minos_hvc(id, a, 0, 0, 0, 0, 0)
#define minos_hvc2(id, a, b)			minos_hvc(id, a, b, 0, 0, 0, 0)
#define minos_hvc3(id, a, b, c) 		minos_hvc(id, a, b, c, 0, 0, 0)
#define minos_hvc4(id, a, b, c, d)		minos_hvc(id, a, b, c, d, 0, 0)
#define minos_hvc5(id, a, b, c, d, e)		minos_hvc(id, a, b, c, d, e, 0)
#define minos_hvc6(id, a, b, c, d, e, f)	minos_hvc(id, a, b, c, d, e, f)

static inline int hvc_vm_create(struct vmtag *vmtag)
{
	return minos_hvc1(HVC_VM_CREATE, vmtag);
}

static inline int hvc_vm_destroy(int vmid)
{
	return minos_hvc1(HVC_VM_DESTORY, vmid);
}

static inline int hvc_vm_reset(int vmid)
{
	return minos_hvc1(HVC_VM_RESTART, vmid);
}

static inline int hvc_vm_power_up(int vmid)
{
	return minos_hvc1(HVC_VM_POWER_UP, vmid);
}

static inline int hvc_vm_power_down(int vmid)
{
	return minos_hvc1(HVC_VM_POWER_DOWN, vmid);
}

static inline int hvc_vm_mmap(int vmid, unsigned long offset, unsigned long size)
{
	return minos_hvc3(HVC_VM_MMAP, vmid, offset, size);
}

static inline void hvc_vm_unmap(int vmid)
{
	minos_hvc1(HVC_VM_UNMMAP, vmid);
}

static inline void hvc_send_virq(int vmid, uint32_t virq)
{
	minos_hvc2(HVC_VM_SEND_VIRQ, vmid, virq);
}

static inline void *hvc_create_vmcs(int vmid)
{
	return (void *)minos_hvc1(HVC_VM_CREATE_VMCS, vmid);
}

static inline int hvc_create_vmcs_irq(int vmid, int vcpu_id)
{
	return (int)minos_hvc2(HVC_VM_CREATE_VMCS_IRQ, vmid, vcpu_id);
}

static inline int hvc_virtio_mmio_deinit(int vmid)
{
	return (int)minos_hvc1(HVC_VM_VIRTIO_MMIO_DEINIT, vmid);
}

static inline int hvc_virtio_mmio_init(int vmid, size_t size,
		unsigned long *gbase, unsigned long *hbase)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(HVC_VM_VIRTIO_MMIO_INIT,
			vmid, size, 0, 0, 0, 0, 0, &res);
	*gbase = res.a1;
	*hbase = res.a2;

	return (int)res.a0;
}

static inline int hvc_create_host_vdev(int vmid)
{
	return (int)minos_hvc1(HVC_VM_CREATE_HOST_VDEV, vmid);
}

static inline int hvc_request_virq(int vmid, int base, int nr)
{
	return (int)minos_hvc3(HVC_VM_REQUEST_VIRQ, vmid, base, nr);
}

#endif
