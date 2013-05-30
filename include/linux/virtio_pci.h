#ifndef _LINUX_VIRTIO_PCI_H
#define _LINUX_VIRTIO_PCI_H

#define VIRTIO_PCI_NO_LEGACY
#include <uapi/linux/virtio_pci.h>

/**
 * virtio_pci_find_capability - walk capabilities to find device info.
 * @dev: the pci device
 * @cfg_type: the VIRTIO_PCI_CAP_* value we seek
 * @ioresource_types: IORESOURCE_MEM and/or IORESOURCE_IO.
 *
 * Returns offset of the capability, or 0.
 */
static inline int virtio_pci_find_capability(struct pci_dev *dev, u8 cfg_type,
					     u32 ioresource_types)
{
	int pos;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type_and_bar, type, bar;
		pci_read_config_byte(dev, pos + offsetof(struct virtio_pci_cap,
							 type_and_bar),
				     &type_and_bar);

		type = (type_and_bar >> VIRTIO_PCI_CAP_TYPE_SHIFT) &
			VIRTIO_PCI_CAP_TYPE_MASK;
		bar = (type_and_bar >> VIRTIO_PCI_CAP_BAR_SHIFT) &
			VIRTIO_PCI_CAP_BAR_MASK;

		if (type == cfg_type) {
			if (pci_resource_flags(dev, bar) & ioresource_types)
				return pos;
		}
	}
	return 0;
}
#endif /* _LINUX_VIRTIO_PCI_H */
