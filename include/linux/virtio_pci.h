#ifndef _LINUX_VIRTIO_PCI_H
#define _LINUX_VIRTIO_PCI_H

#define VIRTIO_PCI_NO_LEGACY
#include <uapi/linux/virtio_pci.h>

/* Returns offset of the capability, or 0. */
static inline int virtio_pci_find_capability(struct pci_dev *dev, u8 cfg_type)
{
	int pos;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type;
		pci_read_config_byte(dev, pos + offsetof(struct virtio_pci_cap,
							 cfg_type), &type);
		if (type == cfg_type)
			return pos;
	}
	return 0;
}
#endif /* _LINUX_VIRTIO_PCI_H */
