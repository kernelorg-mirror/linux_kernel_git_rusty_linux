/*
 * Virtio PCI driver
 *
 * This module allows virtio devices to be used over a virtual PCI device.
 * This can be used with QEMU based VMMs like KVM or Xen.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Anthony Liguori  <aliguori@us.ibm.com>
 *
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _UAPI_LINUX_VIRTIO_PCI_H
#define _UAPI_LINUX_VIRTIO_PCI_H

#include <linux/virtio_config.h>

/* A 32-bit r/o bitmask of the features supported by the host */
#define VIRTIO_PCI_LEGACY_HOST_FEATURES		0

/* A 32-bit r/w bitmask of features activated by the guest */
#define VIRTIO_PCI_LEGACY_GUEST_FEATURES	4

/* A 32-bit r/w PFN for the currently selected queue */
#define VIRTIO_PCI_LEGACY_QUEUE_PFN		8

/* A 16-bit r/o queue size for the currently selected queue */
#define VIRTIO_PCI_LEGACY_QUEUE_NUM		12

/* A 16-bit r/w queue selector */
#define VIRTIO_PCI_LEGACY_QUEUE_SEL		14

/* A 16-bit r/w queue notifier */
#define VIRTIO_PCI_LEGACY_QUEUE_NOTIFY		16

/* An 8-bit device status register.  */
#define VIRTIO_PCI_LEGACY_STATUS		18

/* An 8-bit r/o interrupt status register.  Reading the value will return the
 * current contents of the ISR and will also clear it.  This is effectively
 * a read-and-acknowledge. */
#define VIRTIO_PCI_LEGACY_ISR			19

/* MSI-X registers: only enabled if MSI-X is enabled. */
/* A 16-bit vector for configuration changes. */
#define VIRTIO_MSI_LEGACY_CONFIG_VECTOR        20
/* A 16-bit vector for selected queue notifications. */
#define VIRTIO_MSI_LEGACY_QUEUE_VECTOR         22

/* The remaining space is defined by each driver as the per-driver
 * configuration space */
#define VIRTIO_PCI_LEGACY_CONFIG(dev)		((dev)->msix_enabled ? 24 : 20)

/* How many bits to shift physical queue address written to QUEUE_PFN.
 * 12 is historical, and due to x86 page size. */
#define VIRTIO_PCI_LEGACY_QUEUE_ADDR_SHIFT	12

/* The alignment to use between consumer and producer parts of vring.
 * x86 pagesize again. */
#define VIRTIO_PCI_LEGACY_VRING_ALIGN		4096

#ifndef VIRTIO_PCI_NO_LEGACY
/* Don't break compile of old userspace code.  These will go away. */
#warning "Please support virtio_pci non-legacy mode!"
#define VIRTIO_PCI_HOST_FEATURES VIRTIO_PCI_LEGACY_HOST_FEATURES
#define VIRTIO_PCI_GUEST_FEATURES VIRTIO_PCI_LEGACY_GUEST_FEATURES
#define VIRTIO_PCI_QUEUE_PFN VIRTIO_PCI_LEGACY_QUEUE_PFN
#define VIRTIO_PCI_QUEUE_NUM VIRTIO_PCI_LEGACY_QUEUE_NUM
#define VIRTIO_PCI_QUEUE_SEL VIRTIO_PCI_LEGACY_QUEUE_SEL
#define VIRTIO_PCI_QUEUE_NOTIFY VIRTIO_PCI_LEGACY_QUEUE_NOTIFY
#define VIRTIO_PCI_STATUS VIRTIO_PCI_LEGACY_STATUS
#define VIRTIO_PCI_ISR VIRTIO_PCI_LEGACY_ISR
#define VIRTIO_MSI_CONFIG_VECTOR VIRTIO_MSI_LEGACY_CONFIG_VECTOR
#define VIRTIO_MSI_QUEUE_VECTOR VIRTIO_MSI_LEGACY_QUEUE_VECTOR
#define VIRTIO_PCI_CONFIG(dev) VIRTIO_PCI_LEGACY_CONFIG(dev)
#define VIRTIO_PCI_QUEUE_ADDR_SHIFT VIRTIO_PCI_LEGACY_QUEUE_ADDR_SHIFT
#define VIRTIO_PCI_VRING_ALIGN VIRTIO_PCI_LEGACY_VRING_ALIGN
#endif /* ...!KERNEL */

/* Virtio ABI version, this must match exactly */
#define VIRTIO_PCI_ABI_VERSION		0

/* Vector value used to disable MSI for queue */
#define VIRTIO_MSI_NO_VECTOR            0xffff

/* The bit of the ISR which indicates a device configuration change. */
#define VIRTIO_PCI_ISR_CONFIG		0x2

/* IDs for different capabilities.  Must all exist. */
/* FIXME: Do we win from separating ISR, NOTIFY and COMMON? */
/* Common configuration */
#define VIRTIO_PCI_CAP_COMMON_CFG	1
/* Notifications */
#define VIRTIO_PCI_CAP_NOTIFY_CFG	2
/* ISR access */
#define VIRTIO_PCI_CAP_ISR_CFG		3
/* Device specific confiuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG	4

/* This is the PCI capability header: */
struct virtio_pci_cap {
	u8 cap_vndr;	/* Generic PCI field: PCI_CAP_ID_VNDR */
	u8 cap_next;	/* Generic PCI field: next ptr. */
	u8 cfg_type;	/* One of the VIRTIO_PCI_CAP_*_CFG. */
	u8 bar;		/* Where to find it. */
	__le32 offset;	/* Offset within bar. */
	__le32 length;	/* Length. */
};

/* Fields in VIRTIO_PCI_CAP_COMMON_CFG: */
struct virtio_pci_common_cfg {
	/* About the whole device. */
	__le32 device_feature_select;	/* read-write */
	__le32 device_feature;		/* read-only */
	__le32 guest_feature_select;	/* read-write */
	__le32 guest_feature;		/* read-only */
	__le16 msix_config;		/* read-write */
	__le16 num_queues;		/* read-only */
	__u8 device_status;		/* read-write */
	__u8 unused1;
	__le16 unused2;

	/* About a specific virtqueue. */
	__le16 queue_select;	/* read-write */
	__le16 queue_size;	/* read-write, power of 2. */
	__le16 queue_msix_vector;/* read-write */
	__le16 queue_enable;	/* read-write */
	__le64 queue_desc;	/* read-write */
	__le64 queue_avail;	/* read-write */
	__le64 queue_used;	/* read-write */
};
#endif /* _UAPI_LINUX_VIRTIO_PCI_H */
