/*
 * Virtio PCI driver - common code for legacy and non-legacy.
 *
 * Copyright 2011, Rusty Russell IBM Corporation, but based on the
 * older virtio_pci_legacy.c, which was Copyright IBM Corp. 2007.  But
 * most of the interrupt setup code was written by Michael S. Tsirkin.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#define VIRTIO_PCI_NO_LEGACY
#include "virtio_pci-common.h"
#include <linux/virtio_ring.h>

/* the notify function used when creating a virt queue */
void virtio_pci_notify(struct virtqueue *vq)
{
	struct virtio_pci_vq_info *info = vq->priv;

	/* we write the queue's selector into the notification register to
	 * signal the other end */
	iowrite16(vq->index, info->notify);
}

/* Handle a configuration change: Tell driver if it wants to know. */
irqreturn_t virtio_pci_config_changed(int irq, void *opaque)
{
	struct virtio_pci_device *vp_dev = opaque;
	struct virtio_driver *drv;
	drv = container_of(vp_dev->vdev.dev.driver,
			   struct virtio_driver, driver);

	if (drv->config_changed)
		drv->config_changed(&vp_dev->vdev);
	return IRQ_HANDLED;
}

/* Notify all virtqueues on an interrupt. */
irqreturn_t virtio_pci_vring_interrupt(int irq, void *opaque)
{
	struct virtio_pci_device *vp_dev = opaque;
	struct virtio_pci_vq_info *info;
	irqreturn_t ret = IRQ_NONE;
	unsigned long flags;

	spin_lock_irqsave(&vp_dev->lock, flags);
	list_for_each_entry(info, &vp_dev->virtqueues, node) {
		if (vring_interrupt(irq, info->vq) == IRQ_HANDLED)
			ret = IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&vp_dev->lock, flags);

	return ret;
}

/* A small wrapper to also acknowledge the interrupt when it's handled.
 * I really need an EIO hook for the vring so I can ack the interrupt once we
 * know that we'll be handling the IRQ but before we invoke the callback since
 * the callback may notify the host which results in the host attempting to
 * raise an interrupt that we would then mask once we acknowledged the
 * interrupt. */
irqreturn_t virtio_pci_interrupt(int irq, void *opaque)
{
	struct virtio_pci_device *vp_dev = opaque;
	u8 isr;

	/* reading the ISR has the effect of also clearing it so it's very
	 * important to save off the value. */
	isr = ioread8(vp_dev->isr);

	/* It's definitely not us if the ISR was not high */
	if (!isr)
		return IRQ_NONE;

	/* Configuration change?  Tell driver if it wants to know. */
	if (isr & VIRTIO_PCI_ISR_CONFIG)
		virtio_pci_config_changed(irq, opaque);

	return virtio_pci_vring_interrupt(irq, opaque);
}
