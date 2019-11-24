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

#include <linux/virtio.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_config.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include "vmbox_bus.h"

#define vmbox_virtq_wmb()	wmb()
#define vmbox_virtq_rmb()	rmb()
#define vmbox_virtq_mb		mb()

#define vmbox_virtq_used_event(vq) \
	(uint16_t *)&vq->avail->ring[vq->num]
#define vmbox_virtq_avail_event(vq) \
	(uint16_t *)&vq->used->ring[vq->num]

uint8_t const ffs_table[256] = {
	0u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x00 to 0x0F */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x10 to 0x1F */
	5u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x20 to 0x2F */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x30 to 0x3F */
	6u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x40 to 0x4F */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x50 to 0x5F */
	5u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x60 to 0x6F */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x70 to 0x7F */
	7u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x80 to 0x8F */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0x90 to 0x9F */
	5u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0xA0 to 0xAF */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0xB0 to 0xBF */
	6u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0xC0 to 0xCF */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0xD0 to 0xDF */
	5u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, /* 0xE0 to 0xEF */
	4u, 0u, 1u, 0u, 2u, 0u, 1u, 0u, 3u, 0u, 1u, 0u, 2u, 0u, 1u, 0u  /* 0xF0 to 0xFF */
};

void vmbox_virtq_startup(struct vmbox_virtqueue *vq)
{
	vq->status = VMBOX_VIRTQ_STARTED;
	vmbox_virtq_wmb();
}

void vmbox_virtq_shutdown(struct vmbox_virtqueue *vq)
{
	vq->status = VMBOX_VIRTQ_STOPPED;
	vmbox_virtq_wmb();
}

static inline int vmbox_next_desc(struct vring_desc *desc)
{
	return (!(desc->flags & VRING_DESC_F_NEXT)) ? -1 : desc->next;
}

static int inline vmbox_virtq_need_event(uint16_t event_idx,
		uint16_t new_idx, uint16_t old_idx)
{
	return (uint16_t)(new_idx - event_idx - 1) <
				(uint16_t)(new_idx - old_idx);
}

static inline void vmbox_translate_desc(struct vmbox_virtqueue *vq,
		struct vring_desc *desc, struct vmbox_vring_buf *buf)
{
	buf->addr = vq->vring_buf + desc->addr;
	buf->size = desc->len;
}

static inline int vmbox_virtq_get_freebuf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result, int out)
{
	int head;
	struct vring_desc *desc;
	unsigned int i, n, avail, prev;

	*result = 0;

	if (unlikely(vq->broken))
		return -EIO;

	if (vq->num_free < cnt) {
		if (out)
			vmbox_virtq_notify(vq);
		return -ENOSPC;
	}

	i = head = vq->free_head;
	desc = vq->desc;

	for (n = 0; n < cnt; n++) {
		if (out)
			desc[i].flags = VRING_DESC_F_NEXT;
		else
			desc[i].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;

		desc[i].len = vq->vring_size;

		if (buf)
			vmbox_translate_desc(vq, &desc[i], &buf[n]);

		prev = i;
		i = desc[i].next
	}

	desc[prev].flags &= ~VRING_DESC_F_NEXT;

	/* update the free number and the new head */
	vq->vq.num_free -= cnt;
	vq->free_head = i;
	*result = cnt;

	avail = vq->last_avail_idx & (vq->num - 1);
	vq->avail->ring[avail] = head;

	vmbox_virtq_wmb();
	vq->last_avail_idx++;
	vq->avail->idx = vq->last_avail_idx;
	vq->num_added++;

	return head;
}

int vmbox_virtq_get_inbuf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result)
{
	return vmbox_virtq_get_freebuf(vq, buf, cnt, result, 0);
}

int vmbox_virtq_get_outbuf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result)
{
	return vmbox_virtq_get_freebuf(vq, buf, cnt, result, 1);
}

static inline int vmbox_virtq_more_used(struct vmbox_virtqueue *vq)
{
	return vq->last_used_idx != vq->used->idx;
}

int vmbox_virtq_get_used_buf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result)
{
	unsigned int i, j, m;
	uint16_t last_uese;
	struct vmbox_vring_buf *vbuf = buf;

	*result = 0;

	if (unlikely(vq->broken))
		return -EIO;

	if (!vmbox_more_used(vq))
		return -ENOSPC;

	vmbox_virtio_rmb();

	last_used = vq->last_used_idx & (vq->num - 1);
	m = i = vq->used->ring[last_used].id;

	while (vq->desc[m].flags & VRING_DESC_F_NEXT) {
		m = vq->desc[m].next;
		if (buf)
			vmbox_translate_desc(vq, vbuf, &vq->desc[m]);
		vq->num_free++;
		vbuf++;
	}

	/*
	 * update the free_head of the virtqueue and put
	 * the old free_head to the next field
	 */
	vq->desc[m].next = vq->free_head;
	vq->free_head = i;

	/* Plus final descriptor of the ring */
	vq->num_free++;
	vq->last_used_idx++;
	vmbox_virtq_mb();

	return i;
}

/*
 * vmbox_virtq_get_buf used to get a data buffer
 * from other side
 */
int vmbox_virtq_get_avail_buf(struct vmbox_virtqueue *vq,
		struct vmbox_vring_buf *buf, int cnt, int *result)
{
	int ret;
	struct vring_desc *desc;
	unsigned int i, head;
	uint32_t last_avail_idx;
	uint16_t avail_idx;
	uint32_t count;

	vmbox_virtq_rmb();

	*result = 0;
	last_avail_idx = vq->last_avail_idx;
	avail_idx = vq->avail->idx;
	vq->avail_idx = avail_idx;

	/* to avoid uint16_t overflow */
	count = (uint16_t)((uint32_t)avail_idx - last_avail_idx);
	if (count == 0)
		return vq->num;

	if (count > vq->num) {
		pr_err("avail ring out of range %d %d\n",
				avail_idx, last_avail_idx);
		return -EINVAL;
	}

	head = vq->avail->ring[last_avail_idx & (vq->num - 1)];
	if (head >= vq->num) {
		pr_err("avail ring idx out of range\n");
		return -EINVAL;
	}

	ret = 0;
	i = head;

	do {
		if (*result >= cnt) {
			pr_err("avail count bigger than buf count %d %d %d\n",
					avail_idx, last_avail_idx, cnt);
			return -EINVAL;
		}

		if (i >= vq->num) {
			pr_err("desc index %d > %d head = %d\n",
					i, vq->num, head);
			return -EINVAL;
		}

		desc = &vq->desc[i];
		if (desc->flags & VRING_DESC_F_INDIRECT) {
			pr_err("Vmbox virtq desc do not support INDRIECT feature\n");
			return -EINVAL;
		}

		vmbox_translate_desc(vq, desc, &buf[ret];
		ret++;
	} while ((i = vmbox_next_desc(desc)) != -1);

	*result = ret;
	vq->last_avail_idx++;

	return head;
}

void vmbox_virtq_discard_desc(struct vmbox_virtqueue *vq, int n)
{
	vq->last_avail_idx -= n;
	wmb();
}

static int __vmbox_virtq_add_used_n(struct vmbox_virtqueue *vq,
			struct vring_used_elem *heads,
			unsigned int count)
{
	struct vring_used_elem *used;
	uint16_t old, new;
	int start;

	start = vq->last_used_idx & (vq->num - 1);
	used = vq->used->ring + start;

	if (count == 1) {
		used->id = heads[0].id;
		used->len = heads[0].len;
	} else
		memcpy(used, heads, count * sizeof(*used));

	wmb();

	old = vq->last_used_idx;
	new = (vq->last_used_idx += count);

	if (((uint16_t)(new - vq->signalled_used)) < ((uint16_t)(new - old)))
		vq->signalled_used_valid = 0;

	return 0;
}

int vmbox_virtq_add_used_n(struct vmbox_virtqueue *vq,
		struct vring_used_elem *heads,
		unsigned int count)
{
	int start, n, r;

	start = vq->last_used_idx & (vq->num - 1);
	n = vq->num - start;
	if (n < count) {
		r = __vmbox_virtq_add_used_n(vq, heads, n);
		if (r < 0)
			return r;
		heads += n;
		count -= n;
	}

	r = __vmbox_virtq_add_used_n(vq, heads, count);

	vq->used->idx = vq->last_used_idx;

	return r;
}

int vmbox_virtq_add_used(struct vmbox_virtqueue *vq,
		unsigned int head, uint32_t len)
{
	struct vring_used_elem heads = {
		.id = head,
		.len = len,
	};

	return __vmbox_virtq_add_used_n(vq, &heads, 1);
}

static int vmbox_virtq_need_notify(struct vmbox_virtqueue *vq)
{
	uint16_t old, new;
	uint16_t event;
	int notify;

	old = vq->signalled_used;
	notify = vq->signalled_used_valid;
	new = vq->signalled_used = vq->last_used_idx;
	vq->signalled_used_valid = 1;

	if (!notify)
		return 1;

	event = *vmbox_virtq_used_event(vq);

	return vmbox_virtq_need_event(event, new, old);
}

void vmbox_virtq_notify(struct vmbox_virtqueue *vq)
{
	if (vmbox_virtq_need_notify(vq))
		vmbox_device_vring_event(vq->pdata);
}

void vmbox_virtq_add_used_and_signal(struct vmbox_virtqueue *vq,
		unsigned int head, int len)
{
	vmbox_virtq_add_used(vq, head, len);
	vmbox_virtq_notify(vq);
}

void vmbox_virtq_add_used_and_signal_n(struct vmbox_virtqueue *vq,
				struct vring_used_elem *heads,
				unsigned int count)
{
	vmbox_virtq_add_used_n(vq, heads, count);
	vmbox_virtq_notify(vq);
}

void vmbox_virtq_add_avail_and_signal(struct vmbox_virtqueue *vq,
		unsigned int head, int len)
{

}

void vmbox_virtq_consume_descs(struct vmbox_virtqueue *vq)
{
	int index, cnt;
	struct vmbox_vring_buf buf;

	if (vq->direction == VMBOX_VIRTQ_OUT)
		return;

	while (1) {
		index = vmbox_virtq_get_avail_descs(vq, &buf, 1, &cnt);
		if (!cnt)
			break;

		vmbox_virtq_add_used(vq, index, 0);
	}

	vmbox_virtq_notify(vq);
}
