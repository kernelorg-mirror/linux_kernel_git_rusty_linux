/*
 * Virtio PCI driver (legacy mode)
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Anthony Liguori  <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

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

static bool force_nonlegacy;
module_param(force_nonlegacy, bool, 0644);
MODULE_PARM_DESC(force_nonlegacy, "Take over non-legacy virtio devices too");

MODULE_AUTHOR("Anthony Liguori <aliguori@us.ibm.com>");
MODULE_DESCRIPTION("virtio-pci-legacy");
MODULE_LICENSE("GPL");
MODULE_VERSION("1");

/* Qumranet donated their vendor ID for devices 0x1000 thru 0x10FF. */
static DEFINE_PCI_DEVICE_TABLE(virtio_pci_id_table) = {
	{ PCI_DEVICE(0x1af4, PCI_ANY_ID) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, virtio_pci_id_table);

/* virtio config->get_features() implementation */
static u64 vp_get_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	/* We only support 32 feature bits. */
	return ioread32(vp_dev->legacy + VIRTIO_PCI_LEGACY_HOST_FEATURES);
}

/* virtio config->finalize_features() implementation */
static void vp_finalize_features(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	/* We only support 32 feature bits. */
	iowrite32(vdev->features,
		  vp_dev->legacy + VIRTIO_PCI_LEGACY_GUEST_FEATURES);
}

/* Device config access: we use guest endian, as per spec. */
static u8 vp_get8(struct virtio_device *vdev, unsigned offset)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	void __iomem *ioaddr = vp_dev->legacy +
				VIRTIO_PCI_LEGACY_CONFIG(vp_dev) + offset;

	return ioread8(ioaddr);
}

static void vp_set8(struct virtio_device *vdev, unsigned offset, u8 v)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	void __iomem *ioaddr = vp_dev->legacy +
				VIRTIO_PCI_LEGACY_CONFIG(vp_dev) + offset;

	iowrite8(v, ioaddr);
}

/* config->{get,set}_status() implementations */
static u8 vp_get_status(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	return ioread8(vp_dev->legacy + VIRTIO_PCI_LEGACY_STATUS);
}

static void vp_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* We should never be setting status to 0. */
	BUG_ON(status == 0);
	iowrite8(status, vp_dev->legacy + VIRTIO_PCI_LEGACY_STATUS);
}

static void vp_reset(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	/* 0 status means a reset. */
	iowrite8(0, vp_dev->legacy + VIRTIO_PCI_LEGACY_STATUS);
	/* Flush out the status write, and flush in device writes,
	 * including MSi-X interrupts, if any. */
	ioread8(vp_dev->legacy + VIRTIO_PCI_LEGACY_STATUS);
	/* Flush pending VQ/configuration callbacks. */
	virtio_pci_synchronize_vectors(vdev);
}

static struct virtqueue *setup_legacy_vq(struct virtio_device *vdev,
					 unsigned index,
					 void (*callback)(struct virtqueue *vq),
					 const char *name,
					 u16 msix_vec)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	struct virtio_pci_vq_info *info;
	struct virtqueue *vq;
	unsigned long flags, size;
	u16 num;
	int err;

	/* Select the queue we're interested in */
	iowrite16(index, vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_SEL);

	/* Check if queue is either not available or already active. */
	num = ioread16(vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_NUM);
	if (!num || ioread32(vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_PFN))
		return ERR_PTR(-ENOENT);

	/* allocate and fill out our structure the represents an active
	 * queue */
	info = kmalloc(sizeof(struct virtio_pci_vq_info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->msix_vector = msix_vec;

	size = PAGE_ALIGN(vring_size(num, VIRTIO_PCI_LEGACY_VRING_ALIGN));
	info->queue = alloc_pages_exact(size, GFP_KERNEL|__GFP_ZERO);
	if (info->queue == NULL) {
		err = -ENOMEM;
		goto out_info;
	}

	/* activate the queue */
	iowrite32(virt_to_phys(info->queue)>>VIRTIO_PCI_LEGACY_QUEUE_ADDR_SHIFT,
		  vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_PFN);

	/* create the vring */
	vq = vring_new_virtqueue(index, num,
				 VIRTIO_PCI_LEGACY_VRING_ALIGN, vdev,
				 true, info->queue, virtio_pci_notify,
				 callback, name);
	if (!vq) {
		err = -ENOMEM;
		goto out_activate_queue;
	}

	vq->priv = info;
	info->vq = vq;
	info->notify = vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_NOTIFY;

	if (msix_vec != VIRTIO_MSI_NO_VECTOR) {
		iowrite16(msix_vec,
			  vp_dev->legacy + VIRTIO_MSI_LEGACY_QUEUE_VECTOR);
		msix_vec = ioread16(vp_dev->legacy
				    + VIRTIO_MSI_LEGACY_QUEUE_VECTOR);
		if (msix_vec == VIRTIO_MSI_NO_VECTOR) {
			err = -EBUSY;
			goto out_assign;
		}
	}

	if (callback) {
		spin_lock_irqsave(&vp_dev->lock, flags);
		list_add(&info->node, &vp_dev->virtqueues);
		spin_unlock_irqrestore(&vp_dev->lock, flags);
	} else {
		INIT_LIST_HEAD(&info->node);
	}

	return vq;

out_assign:
	vring_del_virtqueue(vq);
out_activate_queue:
	iowrite32(0, vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_PFN);
	free_pages_exact(info->queue, size);
out_info:
	kfree(info);
	return ERR_PTR(err);
}

static void del_legacy_vq(struct virtqueue *vq)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vq->vdev);
	struct virtio_pci_vq_info *info = vq->priv;
	unsigned long flags, size;

	spin_lock_irqsave(&vp_dev->lock, flags);
	list_del(&info->node);
	spin_unlock_irqrestore(&vp_dev->lock, flags);

	iowrite16(vq->index, vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_SEL);

	if (vp_dev->msix_enabled) {
		iowrite16(VIRTIO_MSI_NO_VECTOR,
			  vp_dev->legacy + VIRTIO_MSI_LEGACY_QUEUE_VECTOR);
		/* Flush the write out to device */
		ioread8(vp_dev->legacy + VIRTIO_PCI_LEGACY_ISR);
	}

	vring_del_virtqueue(vq);

	/* Select and deactivate the queue */
	iowrite32(0, vp_dev->legacy + VIRTIO_PCI_LEGACY_QUEUE_PFN);

	size = PAGE_ALIGN(vring_size(vq->vring.num,
				     VIRTIO_PCI_LEGACY_VRING_ALIGN));
	free_pages_exact(info->queue, size);
	kfree(info);
}

/* the config->del_vqs() implementation */
static void vp_del_vqs(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	virtio_pci_del_vqs(vdev, vp_dev->legacy +
			   VIRTIO_MSI_LEGACY_CONFIG_VECTOR,
			   del_legacy_vq);
}

/* the config->find_vqs() implementation */
static int vp_find_vqs(struct virtio_device *vdev, unsigned nvqs,
		       struct virtqueue *vqs[],
		       vq_callback_t *callbacks[],
		       const char *names[])
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return virtio_pci_find_vqs(vdev, nvqs, vqs, callbacks, names,
				   vp_dev->legacy +
				   VIRTIO_MSI_LEGACY_CONFIG_VECTOR,
				   setup_legacy_vq, del_legacy_vq);
}

static const char *vp_bus_name(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);

	return pci_name(vp_dev->pci_dev);
}

static const struct virtio_config_ops virtio_pci_config_ops = {
	.get8		= vp_get8,
	.set8		= vp_set8,
	VIRTIO_CONFIG_OPS_NOCONV,
	.get_status	= vp_get_status,
	.set_status	= vp_set_status,
	.reset		= vp_reset,
	.find_vqs	= vp_find_vqs,
	.del_vqs	= vp_del_vqs,
	.get_features	= vp_get_features,
	.finalize_features = vp_finalize_features,
	.bus_name	= vp_bus_name,
	.set_vq_affinity = virtio_pci_set_vq_affinity,
};

static void virtio_pci_release_dev(struct device *_d)
{
	/*
	 * No need for a release method as we allocate/free
	 * all devices together with the pci devices.
	 * Provide an empty one to avoid getting a warning from core.
	 */
}

/* the PCI probing function */
static int virtio_pci_probe(struct pci_dev *pci_dev,
			    const struct pci_device_id *id)
{
	struct virtio_pci_device *vp_dev;
	int err, cap;

	/* We only own devices >= 0x1000 and <= 0x103f: leave the rest. */
	if (pci_dev->device < 0x1000 || pci_dev->device > 0x103f)
		return -ENODEV;

	if (pci_dev->revision != VIRTIO_PCI_ABI_VERSION) {
		printk(KERN_ERR "virtio_pci_legacy: expected ABI version %d, got %d\n",
		       VIRTIO_PCI_ABI_VERSION, pci_dev->revision);
		return -ENODEV;
	}

	/* We leave modern virtio-pci for the modern driver. */
	cap = virtio_pci_find_capability(pci_dev, VIRTIO_PCI_CAP_COMMON_CFG,
					 IORESOURCE_IO|IORESOURCE_MEM);
	if (cap) {
		if (force_nonlegacy)
			dev_info(&pci_dev->dev,
				 "virtio_pci_legacy: forcing legacy mode!\n");
		else {
			dev_info(&pci_dev->dev,
				 "virtio_pci_legacy: leaving to"
				 " non-legacy driver\n");
			return -ENODEV;
		}
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

	err = pci_request_regions(pci_dev, "virtio-pci-legacy");
	if (err)
		goto out_enable_device;

	vp_dev->legacy = pci_iomap(pci_dev, 0, 0);
	if (vp_dev->legacy == NULL) {
		err = -ENOMEM;
		goto out_req_regions;
	}

	/* Not used for legacy virtio PCI */
	vp_dev->common = NULL;
	vp_dev->device = NULL;
	vp_dev->notify_base = NULL;
	vp_dev->notify_offset_multiplier = 0;
	vp_dev->notify_len = sizeof(u16);
	/* Device config len actually depends on MSI-X: may overestimate */
	vp_dev->device_len = pci_resource_len(pci_dev, 0) - 20;

	/* Setting this lets us share interrupt handlers with virtio_pci */
	vp_dev->isr = vp_dev->legacy + VIRTIO_PCI_LEGACY_ISR;

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
	pci_iounmap(pci_dev, vp_dev->legacy);
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
	pci_iounmap(pci_dev, vp_dev->legacy);
	pci_release_regions(pci_dev);
	pci_disable_device(pci_dev);
	kfree(vp_dev);
}

#ifdef CONFIG_PM
static const struct dev_pm_ops virtio_pci_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(virtio_pci_freeze, virtio_pci_restore)
};
#endif

static struct pci_driver virtio_pci_driver_legacy = {
	.name		= "virtio-pci-legacy",
	.id_table	= virtio_pci_id_table,
	.probe		= virtio_pci_probe,
	.remove		= virtio_pci_remove,
#ifdef CONFIG_PM
	.driver.pm	= &virtio_pci_pm_ops,
#endif
};

module_pci_driver(virtio_pci_driver_legacy);
