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
#include <linux/interrupt.h>

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

/* wait for pending irq handlers */
void virtio_pci_synchronize_vectors(struct virtio_device *vdev)
{
	struct virtio_pci_device *vp_dev = to_vp_device(vdev);
	int i;

	if (vp_dev->intx_enabled)
		synchronize_irq(vp_dev->pci_dev->irq);

	for (i = 0; i < vp_dev->msix_vectors; ++i)
		synchronize_irq(vp_dev->msix_entries[i].vector);
}

static void vp_free_vectors(struct virtio_pci_device *vp_dev,
			    __le16 __iomem *msix_config)
{
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
		iowrite16(VIRTIO_MSI_NO_VECTOR, msix_config);
		/* Flush the write out to device */
		ioread16(msix_config);

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

static int vp_request_msix_vectors(struct virtio_pci_device *vp_dev,
				   int nvectors,
				   __le16 __iomem *msix_config,
				   bool per_vq_vectors)
{
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

	iowrite16(v, msix_config);
	/* Verify we had enough resources to assign the vector */
	v = ioread16(msix_config);
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
	vp_free_vectors(vp_dev, msix_config);
	return err;
}

static int vp_request_intx(struct virtio_pci_device *vp_dev)
{
	int err;

	err = request_irq(vp_dev->pci_dev->irq, virtio_pci_interrupt,
			  IRQF_SHARED, dev_name(&vp_dev->vdev.dev), vp_dev);
	if (!err)
		vp_dev->intx_enabled = 1;
	return err;
}

static int vp_try_to_find_vqs(struct virtio_pci_device *vp_dev,
			      unsigned nvqs,
			      struct virtqueue *vqs[],
			      vq_callback_t *callbacks[],
			      const char *names[],
			      bool use_msix,
			      bool per_vq_vectors,
			      __le16 __iomem *msix_config,
			      virtio_pci_setup_vq_fn *setup_vq,
			      void (*del_vq)(struct virtqueue *vq))
{
	u16 msix_vec;
	int i, err, nvectors, allocated_vectors;

	if (!use_msix) {
		/* Old style: one normal interrupt for change and all vqs. */
		err = vp_request_intx(vp_dev);
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

		err = vp_request_msix_vectors(vp_dev, nvectors, 
					      msix_config, per_vq_vectors);
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
		vqs[i] = setup_vq(vp_dev, i, callbacks[i], names[i], msix_vec);
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
			del_vq(vqs[i]);
			goto error_find;
		}
	}
	return 0;

error_find:
	virtio_pci_del_vqs(vp_dev, msix_config, del_vq);

error_request:
	return err;
}

int virtio_pci_find_vqs(struct virtio_pci_device *vp_dev, unsigned nvqs,
			struct virtqueue *vqs[],
			vq_callback_t *callbacks[],
			const char *names[],
			__le16 __iomem *msix_config,
			virtio_pci_setup_vq_fn *setup_vq,
			void (*del_vq)(struct virtqueue *vq))
{
	int err;

	/* Try MSI-X with one vector per queue. */
	err = vp_try_to_find_vqs(vp_dev, nvqs, vqs, callbacks, names,
				 true, true, msix_config, setup_vq, del_vq);
	if (!err)
		return 0;
	/* Fallback: MSI-X with one vector for config, one shared for queues. */
	err = vp_try_to_find_vqs(vp_dev, nvqs, vqs, callbacks, names,
				 true, false, msix_config, setup_vq, del_vq);
	if (!err)
		return 0;
	/* Finally fall back to regular interrupts. */
	return vp_try_to_find_vqs(vp_dev, nvqs, vqs, callbacks, names,
				  false, false, msix_config, setup_vq, del_vq);
}

void virtio_pci_del_vqs(struct virtio_pci_device *vp_dev,
			__le16 __iomem *msix_config,
			void (*del_vq)(struct virtqueue *vq))
{
	struct virtqueue *vq, *n;
	struct virtio_pci_vq_info *info;

	list_for_each_entry_safe(vq, n, &vp_dev->vdev.vqs, list) {
		info = vq->priv;
		if (vp_dev->per_vq_vectors &&
			info->msix_vector != VIRTIO_MSI_NO_VECTOR)
			free_irq(vp_dev->msix_entries[info->msix_vector].vector,
				 vq);
		del_vq(vq);
	}
	vp_dev->per_vq_vectors = false;

	vp_free_vectors(vp_dev, msix_config);
}

/* Setup the affinity for a virtqueue:
 * - force the affinity for per vq vector
 * - OR over all affinities for shared MSI
 * - ignore the affinity request if we're using INTX
 */
int virtio_pci_set_vq_affinity(struct virtqueue *vq, int cpu)
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

#ifdef CONFIG_PM
int virtio_pci_freeze(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct virtio_pci_device *vp_dev = pci_get_drvdata(pci_dev);
	struct virtio_driver *drv;
	int ret;

	drv = container_of(vp_dev->vdev.dev.driver,
			   struct virtio_driver, driver);

	ret = 0;
	vp_dev->saved_status = vp_dev->vdev.config->get_status(&vp_dev->vdev);
	if (drv && drv->freeze)
		ret = drv->freeze(&vp_dev->vdev);

	if (!ret)
		pci_disable_device(pci_dev);
	return ret;
}

int virtio_pci_restore(struct device *dev)
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
	vp_dev->vdev.config->finalize_features(&vp_dev->vdev);

	if (drv && drv->restore)
		ret = drv->restore(&vp_dev->vdev);

	/* Finally, tell the device we're all set */
	if (!ret)
		vp_dev->vdev.config->set_status(&vp_dev->vdev,
						vp_dev->saved_status);

	return ret;
}
#endif
