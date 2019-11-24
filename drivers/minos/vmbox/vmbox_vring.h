#ifndef __LINUX_VMBOX_VRING_H__
#define __LINUX_VMBOX_VRING_H__

#include <uapi/linux/virtio_ring.h>

struct vmbox_device;

#define VMBOX_VIRTQ_STOPPED	0x0
#define VMBOX_VIRTQ_STARTED	0x1

#define VMBOX_VIRTQ_OUT		0x0
#define VMBOX_VIRTQ_IN		0x1
#define VMBOX_VIRTQ_BOTH	0x2

struct vmbox_vring_buf {
	void *addr;
	size_t size;
};

struct vmbox_virtqueue {
	int num;
	int vring_size;
	int index;
	bool broken;
	void *vring_buf;
	void *pdata;
	int direction;		/* in or out or both */
	int status;		/* the status of this virtqueue */

	int free_head;
	int num_free;
	int numb_added;

	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;

	uint16_t last_avail_idx;
	uint16_t avail_idx;
	uint16_t last_used_idx;
	uint16_t used_flags;
	uint16_t signalled_used;
	uint16_t signalled_used_valid;

	void (*callback)(struct vmbox_virtqueue *);
};

static int inline vmbox_virtq_has_descs(struct vmbox_virtqueue *vq)
{
	return vq->avail->idx != vq->last_avail_idx;
}

void vmbox_virtq_startup(struct vmbox_virtqueue *vq);
void vmbox_virtq_shutdown(struct vmbox_virtqueue *vq);

int vmbox_virtq_get_out_buf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result);
int vmbox_virtq_get_in_buf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result);

void vmbox_virtq_discard_desc(struct vmbox_virtqueue *vq, int n);

int virtq_add_used_n(struct vmbox_virtqueue *vq,
		struct vring_used_elem *heads,
		unsigned int count);

int vmbox_virtq_add_used(struct vmbox_virtqueue *vq,
		unsigned int head, uint32_t len);

void vmbox_virtq_notify(struct vmbox_virtqueue *vq);

void vmbox_virtq_add_used_and_signal(struct vmbox_virtqueue *vq,
		unsigned int head, int len);

void vmbox_virtq_add_used_and_signal_n(struct vmbox_virtqueue *vq,
				struct vring_used_elem *heads,
				unsigned int count);

void vmbox_virtq_add_avail_and_signal(struct vmbox_virtqueue *vq,
		unsigned int head, int len);

void vmbox_virtq_consume_descs(struct vmbox_virtqueue *vq);

#endif
