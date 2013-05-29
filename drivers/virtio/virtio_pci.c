/*
 * Virtio PCI driver
 *
 * This module allows virtio devices to be used over a virtual PCI
 * device.  Copyright 2011, Rusty Russell IBM Corporation, but based
 * on the older virtio_pci_legacy.c, which was Copyright IBM
 * Corp. 2007.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#define VIRTIO_PCI_NO_LEGACY
#include <linux/module.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_pci.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include "virtio_pci-common.h"

MODULE_AUTHOR("Rusty Russell <rusty@rustcorp.com.au>");
MODULE_DESCRIPTION("virtio-pci");
MODULE_LICENSE("GPL");
MODULE_VERSION("2");

/* Qumranet donated their vendor ID for devices 0x1000 thru 0x10FF. */
static DEFINE_PCI_DEVICE_TABLE(virtio_pci_id_table) = {
	{ PCI_DEVICE(0x1af4, PCI_ANY_ID) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, virtio_pci_id_table);

/* There is no general iowrite64.  We use two 32-bit ops. */
static void iowrite64_twopart(u64 val, const __le64 *addr)
{
	iowrite32((u32)val, (__le32 *)addr);
	iowrite32(val >> 32, (__le32 *)addr + 1);
}

/* There is no ioread64.  We use two 32-bit ops. */
static u64 ioread64_twopart(__le64 *addr)
{
	return ioread32(addr) | ((u64)ioread32((__le32 *)addr + 1) << 32);
}

static u64 vp_get_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	u64 features;

	iowrite32(0, &vp_dev->common->device_feature_select);
	features = ioread32(&vp_dev->common->device_feature);
	iowrite32(1, &vp_dev->common->device_feature_select);
	features |= ((u64)ioread32(&vp_dev->common->device_feature) << 32);
	return features;
}

static void vp_finalize_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	iowrite32(0, &vp_dev->common->guest_feature_select);
	iowrite32((u32)vdev->features, &vp_dev->common->guest_feature);
	iowrite32(1, &vp_dev->common->guest_feature_select);
	iowrite32(vdev->features >> 32, &vp_dev->common->guest_feature);
}

/* virtio config is little-endian for virtio_pci (vs guest-endian for legacy) */
static u8 vp_get8(struct virtio_device *vdev, unsigned offset)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return ioread8(vp_dev->device + offset);
}

static void vp_set8(struct virtio_device *vdev, unsigned offset, u8 val)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	iowrite8(val, vp_dev->device + offset);
}

static u16 vp_get16(struct virtio_device *vdev, unsigned offset)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return ioread16(vp_dev->device + offset);
}

static void vp_set16(struct virtio_device *vdev, unsigned offset, u16 val)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	iowrite16(val, vp_dev->device + offset);
}

static u32 vp_get32(struct virtio_device *vdev, unsigned offset)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return ioread32(vp_dev->device + offset);
}

static void vp_set32(struct virtio_device *vdev, unsigned offset, u32 val)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	iowrite32(val, vp_dev->device + offset);
}

static u64 vp_get64(struct virtio_device *vdev, unsigned offset)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return ioread64_twopart(vp_dev->device + offset);
}

static void vp_set64(struct virtio_device *vdev, unsigned offset, u64 val)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	iowrite64_twopart(val, vp_dev->device + offset);
}

/* config->{get,set}_status() implementations */
static u8 vp_get_status(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	return ioread8(&vp_dev->common->device_status);
}

static void vp_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* We should never be setting status to 0. */
	BUG_ON(status == 0);
	iowrite8(status, &vp_dev->common->device_status);
}

/* wait for pending irq handlers */
static void vp_synchronize_vectors(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	int i;

	if (vp_dev->intx_enabled)
		synchronize_irq(vp_dev->pci_dev->irq);

	for (i = 0; i < vp_dev->msix_vectors; ++i)
		synchronize_irq(vp_dev->msix_entries[i].vector);
}

static void vp_reset(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* 0 status means a reset. */
	iowrite8(0, &vp_dev->common->device_status);
	/* Flush out the status write, and flush in device writes,
	 * including MSI-X interrupts, if any. */
	ioread8(&vp_dev->common->device_status);
	/* Flush pending VQ/configuration callbacks. */
	vp_synchronize_vectors(vdev);
}

static void vp_free_vectors(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	int i;

	if (vp_dev->intx_enabled) {
		free_irq(vp_dev->pci_dev->irq, vp_dev);
		vp_dev->intx_enabled = 0;
	}

	for (i = 0; i < vp_dev->msix_used_vectors; ++i)
		free_irq(vp_dev->msix_entries[i].vector, vp_dev);

	for (i = 0; i < vp_dev->msix_vectors; i++)
		if (vp_dev->msix_affinity_masks[i])
			free_cpumask_var(vp_dev->msix_affinity_masks[i]);

	if (vp_dev->msix_enabled) {
		/* Disable the vector used for configuration */
		iowrite16(VIRTIO_MSI_NO_VECTOR, &vp_dev->common->msix_config);
		/* Flush the write out to device */
		ioread16(&vp_dev->common->msix_config);

		pci_disable_msix(vp_dev->pci_dev);
		vp_dev->msix_enabled = 0;
		vp_dev->msix_vectors = 0;
	}

	vp_dev->msix_used_vectors = 0;
	kfree(vp_dev->msix_names);
	vp_dev->msix_names = NULL;
	kfree(vp_dev->msix_entries);
	vp_dev->msix_entries = NULL;
	kfree(vp_dev->msix_affinity_masks);
	vp_dev->msix_affinity_masks = NULL;
}

static int vp_request_msix_vectors(struct virtio_device *vdev, int nvectors,
				   bool per_vq_vectors)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	const char *name = dev_name(&vp_dev->vdev.dev);
	unsigned i, v;
	int err = -ENOMEM;

	vp_dev->msix_entries = kmalloc(nvectors * sizeof *vp_dev->msix_entries,
				       GFP_KERNEL);
	if (!vp_dev->msix_entries)
		goto error;
	vp_dev->msix_names = kmalloc(nvectors * sizeof *vp_dev->msix_names,
				     GFP_KERNEL);
	if (!vp_dev->msix_names)
		goto error;
	vp_dev->msix_affinity_masks
		= kzalloc(nvectors * sizeof *vp_dev->msix_affinity_masks,
			  GFP_KERNEL);
	if (!vp_dev->msix_affinity_masks)
		goto error;
	for (i = 0; i < nvectors; ++i)
		if (!alloc_cpumask_var(&vp_dev->msix_affinity_masks[i],
					GFP_KERNEL))
			goto error;

	for (i = 0; i < nvectors; ++i)
		vp_dev->msix_entries[i].entry = i;

	/* pci_enable_msix returns positive if we can't get this many. */
	err = pci_enable_msix(vp_dev->pci_dev, vp_dev->msix_entries, nvectors);
	if (err > 0)
		err = -ENOSPC;
	if (err)
		goto error;
	vp_dev->msix_vectors = nvectors;
	vp_dev->msix_enabled = 1;

	/* Set the vector used for configuration */
	v = vp_dev->msix_used_vectors;
	snprintf(vp_dev->msix_names[v], sizeof *vp_dev->msix_names,
		 "%s-config", name);
	err = request_irq(vp_dev->msix_entries[v].vector,
			  virtio_pci_config_changed, 0, vp_dev->msix_names[v],
			  vp_dev);
	if (err)
		goto error;
	++vp_dev->msix_used_vectors;

	iowrite16(v, &vp_dev->common->msix_config);
	/* Verify we had enough resources to assign the vector */
	v = ioread16(&vp_dev->common->msix_config);
	if (v == VIRTIO_MSI_NO_VECTOR) {
		err = -EBUSY;
		goto error;
	}

	if (!per_vq_vectors) {
		/* Shared vector for all VQs */
		v = vp_dev->msix_used_vectors;
		snprintf(vp_dev->msix_names[v], sizeof *vp_dev->msix_names,
			 "%s-virtqueues", name);
		err = request_irq(vp_dev->msix_entries[v].vector,
				  virtio_pci_vring_interrupt, 0,
				  vp_dev->msix_names[v], vp_dev);
		if (err)
			goto error;
		++vp_dev->msix_used_vectors;
	}
	return 0;
error:
	vp_free_vectors(vdev);
	return err;
}

static int vp_request_intx(struct virtio_device *vdev)
{
	int err;
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	err = request_irq(vp_dev->pci_dev->irq, virtio_pci_interrupt,
			  IRQF_SHARED, dev_name(&vdev->dev), vp_dev);
	if (!err)
		vp_dev->intx_enabled = 1;
	return err;
}

static size_t vring_pci_size(u16 num)
{
	/* We only need a cacheline separation. */
	return PAGE_ALIGN(vring_size(num, SMP_CACHE_BYTES));
}

static void *alloc_virtqueue_pages(u16 *num)
{
	void *pages;

	/* 1024 entries uses about 32k */
	if (*num > 1024)
		*num = 1024;

	for (; *num; *num /= 2) {
		pages = alloc_pages_exact(vring_pci_size(*num),
					  GFP_KERNEL|__GFP_ZERO|__GFP_NOWARN);
		if (pages)
			return pages;
	}
	return NULL;
}

static struct virtqueue *setup_vq(struct virtio_device *vdev, unsigned index,
				  void (*callback)(struct virtqueue *vq),
				  const char *name,
				  u16 msix_vec)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtio_pci_vq_info *info;
	struct virtqueue *vq;
	u16 num, off;
	int err;

	if (index >= ioread16(&vp_dev->common->num_queues))
		return ERR_PTR(-ENOENT);

	/* Select the queue we're interested in */
	iowrite16(index, &vp_dev->common->queue_select);

	/* Sanity check */
	switch (ioread64_twopart(&vp_dev->common->queue_desc)) {
		/* Uninitialized.  Excellent. */
		break;
	default:
		/* We've already set this up? */
		return ERR_PTR(-EBUSY);
	}

	/* Maximum size must be a power of 2. */
	num = ioread16(&vp_dev->common->queue_size);
	if (num & (num - 1)) {
		dev_warn(&vp_dev->pci_dev->dev, "bad queue size %u", num);
		return ERR_PTR(-EINVAL);
	}

	/* allocate and fill out our structure the represents an active
	 * queue */
	info = kmalloc(sizeof(struct virtio_pci_vq_info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->msix_vector = msix_vec;

	/* get offset of notification word for this vq (shouldn't wrap) */
	off = ioread16(&vp_dev->common->queue_notify_off);
	if ((u64)off * vp_dev->notify_offset_multiplier + 2
	    > vp_dev->notify_len) {
		dev_warn(&vp_dev->pci_dev->dev,
			 "bad notification offset %u (x %u) for queue %u > %u",
			 off, vp_dev->notify_offset_multiplier, 
			 index, vp_dev->notify_len);
		err = -EINVAL;
		goto out_info;
	}
	info->notify = vp_dev->notify_base + off * vp_dev->notify_offset_multiplier;

	info->queue = alloc_virtqueue_pages(&num);
	if (info->queue == NULL) {
		err = -ENOMEM;
		goto out_info;
	}

	/* create the vring */
	vq = vring_new_virtqueue(index, num, SMP_CACHE_BYTES, vdev,
				 true, info->queue, virtio_pci_notify,
				 callback, name);
	if (!vq) {
		err = -ENOMEM;
		goto out_alloc_pages;
	}

	vq->priv = info;
	info->vq = vq;

	if (msix_vec != VIRTIO_MSI_NO_VECTOR) {
		iowrite16(msix_vec, &vp_dev->common->queue_msix_vector);
		msix_vec = ioread16(&vp_dev->common->queue_msix_vector);
		if (msix_vec == VIRTIO_MSI_NO_VECTOR) {
			err = -EBUSY;
			goto out_new_virtqueue;
		}
	}

	if (callback) {
		unsigned long flags;
		spin_lock_irqsave(&vp_dev->lock, flags);
		list_add(&info->node, &vp_dev->virtqueues);
		spin_unlock_irqrestore(&vp_dev->lock, flags);
	} else {
		INIT_LIST_HEAD(&info->node);
	}

	/* Activate the queue. */
	iowrite16(num, &vp_dev->common->queue_size);
	iowrite64_twopart(virt_to_phys(vq->vring.desc),
			  &vp_dev->common->queue_desc);
	iowrite64_twopart(virt_to_phys(vq->vring.avail),
			  &vp_dev->common->queue_avail);
	iowrite64_twopart(virt_to_phys(vq->vring.used),
			  &vp_dev->common->queue_used);
	iowrite8(1, &vp_dev->common->queue_enable);

	return vq;

out_new_virtqueue:
	vring_del_virtqueue(vq);
out_alloc_pages:
	free_pages_exact(info->queue, vring_pci_size(num));
out_info:
	kfree(info);
	return ERR_PTR(err);
}

static void vp_vq_disable(struct virtio_pci_device *vp_dev,
			  struct virtqueue *vq)
{
	unsigned long end;

	/* Select the queue */
	iowrite16(vq->index, &vp_dev->common->queue_select);

	/* Disable it */
 	iowrite16(0, &vp_dev->common->queue_enable);

	/* It's almost certainly synchronous, but just in case. */
	end = jiffies + HZ/2;
	while (ioread16(&vp_dev->common->queue_enable) != 0) {
		if (time_after(jiffies, end)) {
			dev_warn(&vp_dev->pci_dev->dev,
				 "virtio_pci: disable ignored\n");
			break;
		}
		cpu_relax();
	}
}

static void vp_del_vq(struct virtqueue *vq)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vq->vdev);
	struct virtio_pci_vq_info *info = vq->priv;
	unsigned long flags, size = vring_pci_size(vq->vring.num);

	spin_lock_irqsave(&vp_dev->lock, flags);
	list_del(&info->node);
	spin_unlock_irqrestore(&vp_dev->lock, flags);

	/* It should be quiescent, but disable first just in case. */
	vp_vq_disable(vp_dev, vq);

	/* Select the queue */
	iowrite16(vq->index, &vp_dev->common->queue_select);

	if (vp_dev->msix_enabled) {
		iowrite16(VIRTIO_MSI_NO_VECTOR,
			  &vp_dev->common->queue_msix_vector);
		/* Flush the write out to device */
		ioread16(&vp_dev->common->queue_msix_vector);
	}

	vring_del_virtqueue(vq);

	/* This is for our own benefit, not the device's! */
	iowrite16(0, &vp_dev->common->queue_size);
	iowrite64_twopart(0, &vp_dev->common->queue_desc);
	iowrite64_twopart(0, &vp_dev->common->queue_avail);
	iowrite64_twopart(0, &vp_dev->common->queue_used);

	free_pages_exact(info->queue, size);
	kfree(info);
}

/* the config->del_vqs() implementation */
static void vp_del_vqs(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtqueue *vq, *n;
	struct virtio_pci_vq_info *info;

	list_for_each_entry_safe(vq, n, &vdev->vqs, list) {
		info = vq->priv;
		if (vp_dev->per_vq_vectors &&
			info->msix_vector != VIRTIO_MSI_NO_VECTOR)
			free_irq(vp_dev->msix_entries[info->msix_vector].vector,
				 vq);
		vp_del_vq(vq);
	}
	vp_dev->per_vq_vectors = false;

	vp_free_vectors(vdev);
}

static int vp_try_to_find_vqs(struct virtio_device *vdev, unsigned nvqs,
			      struct virtqueue *vqs[],
			      vq_callback_t *callbacks[],
			      const char *names[],
			      bool use_msix,
			      bool per_vq_vectors)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	u16 msix_vec;
	int i, err, nvectors, allocated_vectors;

	if (!use_msix) {
		/* Old style: one normal interrupt for change and all vqs. */
		err = vp_request_intx(vdev);
		if (err)
			goto error_request;
	} else {
		if (per_vq_vectors) {
			/* Best option: one for change interrupt, one per vq. */
			nvectors = 1;
			for (i = 0; i < nvqs; ++i)
				if (callbacks[i])
					++nvectors;
		} else {
			/* Second best: one for change, shared for all vqs. */
			nvectors = 2;
		}

		err = vp_request_msix_vectors(vdev, nvectors, per_vq_vectors);
		if (err)
			goto error_request;
	}

	vp_dev->per_vq_vectors = per_vq_vectors;
	allocated_vectors = vp_dev->msix_used_vectors;
	for (i = 0; i < nvqs; ++i) {
		if (!names[i]) {
			vqs[i] = NULL;
			continue;
		} else if (!callbacks[i] || !vp_dev->msix_enabled)
			msix_vec = VIRTIO_MSI_NO_VECTOR;
		else if (vp_dev->per_vq_vectors)
			msix_vec = allocated_vectors++;
		else
			msix_vec = VP_MSIX_VQ_VECTOR;
		vqs[i] = setup_vq(vdev, i, callbacks[i], names[i], msix_vec);
		if (IS_ERR(vqs[i])) {
			err = PTR_ERR(vqs[i]);
			goto error_find;
		}

		if (!vp_dev->per_vq_vectors || msix_vec == VIRTIO_MSI_NO_VECTOR)
			continue;

		/* allocate per-vq irq if available and necessary */
		snprintf(vp_dev->msix_names[msix_vec],
			 sizeof *vp_dev->msix_names,
			 "%s-%s",
			 dev_name(&vp_dev->vdev.dev), names[i]);
		err = request_irq(vp_dev->msix_entries[msix_vec].vector,
				  vring_interrupt, 0,
				  vp_dev->msix_names[msix_vec],
				  vqs[i]);
		if (err) {
			vp_del_vq(vqs[i]);
			goto error_find;
		}
	}
	return 0;

error_find:
	vp_del_vqs(vdev);

error_request:
	return err;
}

/* the config->find_vqs() implementation */
static int vp_find_vqs(struct virtio_device *vdev, unsigned nvqs,
		       struct virtqueue *vqs[],
		       vq_callback_t *callbacks[],
		       const char *names[])
{
	int err;

	/* Try MSI-X with one vector per queue. */
	err = vp_try_to_find_vqs(vdev, nvqs, vqs, callbacks, names, true, true);
	if (!err)
		return 0;
	/* Fallback: MSI-X with one vector for config, one shared for queues. */
	err = vp_try_to_find_vqs(vdev, nvqs, vqs, callbacks, names,
				 true, false);
	if (!err)
		return 0;
	/* Finally fall back to regular interrupts. */
	return vp_try_to_find_vqs(vdev, nvqs, vqs, callbacks, names,
				  false, false);
}

static const char *vp_bus_name(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return pci_name(vp_dev->pci_dev);
}

/* Setup the affinity for a virtqueue:
 * - force the affinity for per vq vector
 * - OR over all affinities for shared MSI
 * - ignore the affinity request if we're using INTX
 */
static int vp_set_vq_affinity(struct virtqueue *vq, int cpu)
{
	struct virtio_device *vdev = vq->vdev;
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtio_pci_vq_info *info = vq->priv;
	struct cpumask *mask;
	unsigned int irq;

	if (!vq->callback)
		return -EINVAL;

	if (vp_dev->msix_enabled) {
		mask = vp_dev->msix_affinity_masks[info->msix_vector];
		irq = vp_dev->msix_entries[info->msix_vector].vector;
		if (cpu == -1)
			irq_set_affinity_hint(irq, NULL);
		else {
			cpumask_set_cpu(cpu, mask);
			irq_set_affinity_hint(irq, mask);
		}
	}
	return 0;
}

static const struct virtio_config_ops virtio_pci_config_ops = {
	.get8		= vp_get8,
	.set8		= vp_set8,
	.get16		= vp_get16,
	.set16		= vp_set16,
	.get32		= vp_get32,
	.set32		= vp_set32,
	.get64		= vp_get64,
	.set64		= vp_set64,
	.get_status	= vp_get_status,
	.set_status	= vp_set_status,
	.reset		= vp_reset,
	.find_vqs	= vp_find_vqs,
	.del_vqs	= vp_del_vqs,
	.get_features	= vp_get_features,
	.finalize_features = vp_finalize_features,
	.bus_name	= vp_bus_name,
	.set_vq_affinity = vp_set_vq_affinity,
};

static void virtio_pci_release_dev(struct device *_d)
{
	/*
	 * No need for a release method as we allocate/free
	 * all devices together with the pci devices.
	 * Provide an empty one to avoid getting a warning from core.
	 */
}

static void __iomem *map_capability(struct pci_dev *dev, int off, size_t minlen,
				    size_t *len)
{
	u8 bar;
	u32 offset, length;
	void __iomem *p;

	pci_read_config_byte(dev, off + offsetof(struct virtio_pci_cap, bar),
			     &bar);
	pci_read_config_dword(dev, off + offsetof(struct virtio_pci_cap, offset),
			     &offset);
	pci_read_config_dword(dev, off + offsetof(struct virtio_pci_cap, length),
			     &length);

	if (length < minlen) {
		dev_err(&dev->dev,
			"virtio_pci: small capability len %u (%zu expected)\n",
			length, minlen);
		return NULL;
	}

	if (len)
		*len = length;

	/* We want uncachable mapping, even if bar is cachable. */
	p = pci_iomap_range(dev, bar, offset, length, PAGE_SIZE, true);
	if (!p)
		dev_err(&dev->dev,
			"virtio_pci: unable to map virtio %u@%u on bar %i\n",
			length, offset, bar);
	return p;
}


/* the PCI probing function */
static int virtio_pci_probe(struct pci_dev *pci_dev,
			    const struct pci_device_id *id)
{
	struct virtio_pci_device *vp_dev;
	int err, common, isr, notify, device;

	/* We only own devices >= 0x1000 and <= 0x103f: leave the rest. */
	if (pci_dev->device < 0x1000 || pci_dev->device > 0x103f)
		return -ENODEV;

	if (pci_dev->revision != VIRTIO_PCI_ABI_VERSION) {
		printk(KERN_ERR "virtio_pci: expected ABI version %d, got %d\n",
		       VIRTIO_PCI_ABI_VERSION, pci_dev->revision);
		return -ENODEV;
	}

	/* check for a common config: if not, use legacy mode (bar 0). */
	common = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_COMMON_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	if (!common) {
		dev_info(&pci_dev->dev,
			 "virtio_pci: leaving for legacy driver\n");
		return -ENODEV;
	}

	/* If common is there, these should be too... */
	isr = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_ISR_CFG,
					 IORESOURCE_IO|IORESOURCE_MEM);
	notify = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_NOTIFY_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	device = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_DEVICE_CFG,
					    IORESOURCE_IO|IORESOURCE_MEM);
	if (!isr || !notify || !device) {
		dev_err(&pci_dev->dev,
			"virtio_pci: missing capabilities %i/%i/%i/%i\n",
			common, isr, notify, device);
		return -EINVAL;
	}

	/* allocate our structure and fill it out */
	vp_dev = kzalloc(sizeof(struct virtio_pci_device), GFP_KERNEL);
	if (vp_dev == NULL)
		return -ENOMEM;

	vp_dev->vdev.dev.parent = &pci_dev->dev;
	vp_dev->vdev.dev.release = virtio_pci_release_dev;
	vp_dev->vdev.config = &virtio_pci_config_ops;
	vp_dev->pci_dev = pci_dev;
	INIT_LIST_HEAD(&vp_dev->virtqueues);
	spin_lock_init(&vp_dev->lock);

	/* Disable MSI/MSIX to bring device to a known good state. */
	pci_msi_off(pci_dev);

	/* enable the device */
	err = pci_enable_device(pci_dev);
	if (err)
		goto out;

	err = pci_request_regions(pci_dev, "virtio-pci");
	if (err)
		goto out_enable_device;

	err = -EINVAL;
	vp_dev->common = map_capability(pci_dev, common,
					sizeof(struct virtio_pci_common_cfg),
					NULL);
	if (!vp_dev->common)
		goto out_req_regions;
	vp_dev->isr = map_capability(pci_dev, isr, sizeof(u8), NULL);
	if (!vp_dev->isr)
		goto out_map_common;

	/* Read notify_off_multiplier from config space. */
	pci_read_config_dword(pci_dev,
			      notify + offsetof(struct virtio_pci_notify_cap,
						notify_off_multiplier),
			      &vp_dev->notify_offset_multiplier);
	vp_dev->notify_base = map_capability(pci_dev, notify, sizeof(u8),
					     &vp_dev->notify_len);
	if (!vp_dev->notify_len)
		goto out_map_isr;
	vp_dev->device = map_capability(pci_dev, device, 0, &vp_dev->device_len);
	if (!vp_dev->device)
		goto out_map_notify;

	pci_set_drvdata(pci_dev, vp_dev);
	pci_set_master(pci_dev);

	/* we use the subsystem vendor/device id as the virtio vendor/device
	 * id.  this allows us to use the same PCI vendor/device id for all
	 * virtio devices and to identify the particular virtio driver by
	 * the subsystem ids */
	vp_dev->vdev.id.vendor = pci_dev->subsystem_vendor;
	vp_dev->vdev.id.device = pci_dev->subsystem_device;

	/* finally register the virtio device */
	err = register_virtio_device(&vp_dev->vdev);
	if (err)
		goto out_set_drvdata;

	return 0;

out_set_drvdata:
	pci_set_drvdata(pci_dev, NULL);
	pci_iounmap(pci_dev, vp_dev->device);
out_map_notify:
	pci_iounmap(pci_dev, vp_dev->notify_base);
out_map_isr:
	pci_iounmap(pci_dev, vp_dev->isr);
out_map_common:
	pci_iounmap(pci_dev, vp_dev->common);
out_req_regions:
	pci_release_regions(pci_dev);
out_enable_device:
	pci_disable_device(pci_dev);
out:
	kfree(vp_dev);
	return err;
}

static void virtio_pci_remove(struct pci_dev *pci_dev)
{
	struct virtio_pci_device *vp_dev = pci_get_drvdata(pci_dev);

	unregister_virtio_device(&vp_dev->vdev);

	vp_del_vqs(&vp_dev->vdev);
	pci_set_drvdata(pci_dev, NULL);
	pci_iounmap(pci_dev, vp_dev->device);
	pci_iounmap(pci_dev, vp_dev->notify_base);
	pci_iounmap(pci_dev, vp_dev->isr);
	pci_iounmap(pci_dev, vp_dev->common);
	pci_release_regions(pci_dev);
	pci_disable_device(pci_dev);
	kfree(vp_dev);
}

#ifdef CONFIG_PM
static int virtio_pci_freeze(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct virtio_pci_device *vp_dev = pci_get_drvdata(pci_dev);
	struct virtio_driver *drv;
	int ret;

	drv = container_of(vp_dev->vdev.dev.driver,
			   struct virtio_driver, driver);

	ret = 0;
	vp_dev->saved_status = vp_get_status(&vp_dev->vdev);
	if (drv && drv->freeze)
		ret = drv->freeze(&vp_dev->vdev);

	if (!ret)
		pci_disable_device(pci_dev);
	return ret;
}

static int virtio_pci_restore(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct virtio_pci_device *vp_dev = pci_get_drvdata(pci_dev);
	struct virtio_driver *drv;
	int ret;

	drv = container_of(vp_dev->vdev.dev.driver,
			   struct virtio_driver, driver);

	ret = pci_enable_device(pci_dev);
	if (ret)
		return ret;

	pci_set_master(pci_dev);
	vp_finalize_features(&vp_dev->vdev);

	if (drv && drv->restore)
		ret = drv->restore(&vp_dev->vdev);

	/* Finally, tell the device we're all set */
	if (!ret)
		vp_set_status(&vp_dev->vdev, vp_dev->saved_status);

	return ret;
}

static const struct dev_pm_ops virtio_pci_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(virtio_pci_freeze, virtio_pci_restore)
};
#endif

static struct pci_driver virtio_pci_driver = {
	.name		= "virtio-pci",
	.id_table	= virtio_pci_id_table,
	.probe		= virtio_pci_probe,
	.remove		= virtio_pci_remove,
#ifdef CONFIG_PM
 	.driver.pm	= &virtio_pci_pm_ops,
#endif
};

module_pci_driver(virtio_pci_driver);
