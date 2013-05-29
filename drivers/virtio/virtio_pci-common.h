#include <linux/pci.h>
#include <linux/virtio_pci.h>

/* Our device structure: shared by virtio_pci and virtio_pci_legacy. */
struct virtio_pci_device {
	struct virtio_device vdev;
	struct pci_dev *pci_dev;

	/* The IO mapping for the PCI config space (non-legacy mode) */
	struct virtio_pci_common_cfg __iomem *common;
	/* Device-specific data (non-legacy mode)  */
	void __iomem *device;
	/* Base of vq notifications (non-legacy mode). */
	void __iomem *notify_base;

	/* In legacy mode, these two point to within ->legacy. */
	/* Where to read and clear interrupt */
	u8 __iomem *isr;

	/* So we can sanity-check accesses. */
	size_t notify_len;
	size_t device_len;

	/* Multiply queue_notify_off by this value. (non-legacy mode). */
	u32 notify_offset_multiplier;

	/* a list of queues so we can dispatch IRQs */
	spinlock_t lock;
	struct list_head virtqueues;

	/* MSI-X support */
	int msix_enabled;
	int intx_enabled;
	struct msix_entry *msix_entries;
	cpumask_var_t *msix_affinity_masks;
	/* Name strings for interrupts. This size should be enough,
	 * and I'm too lazy to allocate each name separately. */
	char (*msix_names)[256];
	/* Number of available vectors */
	unsigned msix_vectors;
	/* Vectors allocated, excluding per-vq vectors if any */
	unsigned msix_used_vectors;

	/* Status saved during hibernate/restore */
	u8 saved_status;

	/* Whether we have vector per vq */
	bool per_vq_vectors;

#ifdef CONFIG_VIRTIO_PCI_LEGACY
	/* Instead of common and device, legacy uses this: */
	void __iomem *legacy;
#endif
};

/* Constants for MSI-X */
/* Use first vector for configuration changes, second and the rest for
 * virtqueues Thus, we need at least 2 vectors for MSI. */
enum {
	VP_MSIX_CONFIG_VECTOR = 0,
	VP_MSIX_VQ_VECTOR = 1,
};

struct virtio_pci_vq_info {
	/* the actual virtqueue */
	struct virtqueue *vq;

	/* the pages used for the queue. */
	void *queue;

	/* the list node for the virtqueues list */
	struct list_head node;

	/* Notify area for this vq. */
	u16 __iomem *notify;

	/* MSI-X vector (or none) */
	unsigned msix_vector;
};
