/* Copyright (C) 2009 Red Hat, Inc.
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * virtio-net server in host kernel.
 */

#include <linux/compat.h>
#include <linux/eventfd.h>
#include <linux/vhost.h>
#include <linux/virtio_net.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/file.h>
#include <linux/slab.h>

#include <linux/net.h>
#include <linux/if_packet.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <linux/if_macvlan.h>
#include <linux/if_vlan.h>

#include <net/sock.h>

#include "vhost.h"

static int experimental_zcopytx = 1;
module_param(experimental_zcopytx, int, 0444);
MODULE_PARM_DESC(experimental_zcopytx, "Enable Zero Copy TX;"
		                       " 1 -Enable; 0 - Disable");

/* Max number of bytes transferred before requeueing the job.
 * Using this limit prevents one virtqueue from starving others. */
#define VHOST_NET_WEIGHT 0x80000

/* MAX number of TX used buffers for outstanding zerocopy */
#define VHOST_MAX_PEND 128
#define VHOST_GOODCOPY_LEN 256

/*
 * For transmit, used buffer len is unused; we override it to track buffer
 * status internally; used for zerocopy tx only.
 */
/* Lower device DMA failed */
#define VHOST_DMA_FAILED_LEN	3
/* Lower device DMA done */
#define VHOST_DMA_DONE_LEN	2
/* Lower device DMA in progress */
#define VHOST_DMA_IN_PROGRESS	1
/* Buffer unused */
#define VHOST_DMA_CLEAR_LEN	0

#define VHOST_DMA_IS_DONE(len) ((len) >= VHOST_DMA_DONE_LEN)

enum {
	VHOST_NET_FEATURES = VHOST_FEATURES |
			 (1ULL << VHOST_NET_F_VIRTIO_NET_HDR) |
			 (1ULL << VIRTIO_NET_F_MRG_RXBUF),
};

enum {
	VHOST_NET_VQ_RX = 0,
	VHOST_NET_VQ_TX = 1,
	VHOST_NET_VQ_MAX = 2,
};

struct vhost_net_ubuf_ref {
	struct kref kref;
	wait_queue_head_t wait;
	struct vhost_net_virtqueue *nvq;
	/* Finished buffers, protected by lock. */
	spinlock_t completed_lock;
	struct list_head completed;
};

struct vhost_ubuf_info {
	struct ubuf_info ubuf;
	struct vhost_net_ubuf_ref *ubufs;
	int id;
	/* Once we're complete, we know if we zero copied or not. */
	bool success;
	/* Once we're complete, we get put into ubufs->completed. */
	struct list_head list;
};

struct vhost_net_virtqueue {
	struct vhost_virtqueue vq;
	/* hdr is used to store the virtio header.
	 * Since each iovec has >= 1 byte length, we never need more than
	 * header length entries to store the header. */
	struct iovec hdr[sizeof(struct virtio_net_hdr_mrg_rxbuf)];
	size_t vhost_hlen;
	size_t sock_hlen;
	/* Reference counting for outstanding ubufs.
	 * Protected by vq mutex. Writers must also take device mutex. */
	struct vhost_net_ubuf_ref *ubufs;
};

struct vhost_net {
	struct vhost_dev dev;
	struct vhost_net_virtqueue vqs[VHOST_NET_VQ_MAX];
	struct vhost_poll poll[VHOST_NET_VQ_MAX];
	/* Number of TX recently submitted.
	 * Protected by tx vq lock. */
	unsigned tx_packets;
	/* Number of times zerocopy TX recently failed.
	 * Protected by tx vq lock. */
	unsigned tx_zcopy_err;
	/* Paused.  Don't do anything. */
	unsigned paused;
};

static unsigned vhost_net_zcopy_mask __read_mostly;
static struct kmem_cache *ubuf_cache __read_mostly;

static void vhost_net_enable_zcopy(int vq)
{
	vhost_net_zcopy_mask |= 0x1 << vq;
}

static void vhost_net_zerocopy_done_signal(struct kref *kref)
{
	struct vhost_net_ubuf_ref *ubufs;

	ubufs = container_of(kref, struct vhost_net_ubuf_ref, kref);
	wake_up(&ubufs->wait);
}

static struct vhost_net_ubuf_ref *
vhost_net_ubuf_alloc(struct vhost_net_virtqueue *nvq, bool zcopy)
{
	struct vhost_net_ubuf_ref *ubufs;
	/* No zero copy backend? Nothing to count. */
	if (!zcopy)
		return NULL;
	ubufs = kmalloc(sizeof(*ubufs), GFP_KERNEL);
	if (!ubufs)
		return ERR_PTR(-ENOMEM);
	kref_init(&ubufs->kref);
	init_waitqueue_head(&ubufs->wait);
	spin_lock_init(&ubufs->completed_lock);
	INIT_LIST_HEAD(&ubufs->completed);
	ubufs->nvq = nvq;
	return ubufs;
}

static void vhost_net_ubuf_put(struct vhost_net_ubuf_ref *ubufs)
{
	kref_put(&ubufs->kref, vhost_net_zerocopy_done_signal);
}

static void vhost_net_ubuf_put_and_wait(struct vhost_net_ubuf_ref *ubufs)
{
	kref_put(&ubufs->kref, vhost_net_zerocopy_done_signal);
	wait_event(ubufs->wait, !atomic_read(&ubufs->kref.refcount));
}

static void vhost_net_ubuf_put_wait_and_free(struct vhost_net_ubuf_ref *ubufs)
{
	vhost_net_ubuf_put_and_wait(ubufs);
	kfree(ubufs);
}

static void vhost_net_vq_reset(struct vhost_net *n)
{
	int i;

	for (i = 0; i < VHOST_NET_VQ_MAX; i++) {
		n->vqs[i].ubufs = NULL;
		n->vqs[i].vhost_hlen = 0;
		n->vqs[i].sock_hlen = 0;
	}

}

static void vhost_net_tx_packet(struct vhost_net *net)
{
	++net->tx_packets;
	if (net->tx_packets < 1024)
		return;
	net->tx_packets = 0;
	net->tx_zcopy_err = 0;
}

static void vhost_net_tx_err(struct vhost_net *net)
{
	++net->tx_zcopy_err;
}

static bool vhost_net_tx_select_zcopy(struct vhost_net *net)
{
	return net->tx_packets / 64 >= net->tx_zcopy_err;
}

static bool vhost_sock_zcopy(struct socket *sock)
{
	return unlikely(experimental_zcopytx) &&
		sock_flag(sock->sk, SOCK_ZEROCOPY);
}

/* Pop first len bytes from iovec. Return number of segments used. */
static int move_iovec_hdr(struct iovec *from, struct iovec *to,
			  size_t len, int iov_count)
{
	int seg = 0;
	size_t size;

	while (len && seg < iov_count) {
		size = min(from->iov_len, len);
		to->iov_base = from->iov_base;
		to->iov_len = size;
		from->iov_len -= size;
		from->iov_base += size;
		len -= size;
		++from;
		++to;
		++seg;
	}
	return seg;
}
/* Copy iovec entries for len bytes from iovec. */
static void copy_iovec_hdr(const struct iovec *from, struct iovec *to,
			   size_t len, int iovcount)
{
	int seg = 0;
	size_t size;

	while (len && seg < iovcount) {
		size = min(from->iov_len, len);
		to->iov_base = from->iov_base;
		to->iov_len = size;
		len -= size;
		++from;
		++to;
		++seg;
	}
}

/*
 * FIXME: kmap the pages so we can do vhost_add_used() directly in
 * vhost_zerocopy_callback, and just do the vhost_signal() here.
 */
static void vhost_zerocopy_signal_used(struct vhost_net *net,
				       struct vhost_virtqueue *vq)
{
	struct list_head done = LIST_HEAD_INIT(done);
	unsigned long flags;
	bool some_used = false;
	struct vhost_ubuf_info *i, *next;
	struct vhost_net_virtqueue *nvq =
		container_of(vq, struct vhost_net_virtqueue, vq);

	/* Empty the list */
	spin_lock_irqsave(&nvq->ubufs->completed_lock, flags);
	list_splice_init(&nvq->ubufs->completed, &done);
	spin_unlock_irqrestore(&nvq->ubufs->completed_lock, flags);

	list_for_each_entry_safe(i, next, &done, list) {
		vhost_add_used(vq, i->id, 0);
		/* FIXME: Do this in vhost_zerocopy_callback itself. */
		if (!i->success)
			vhost_net_tx_err(net);
		kmem_cache_free(ubuf_cache, i);
		vhost_net_ubuf_put(nvq->ubufs);
		some_used = true;
	}
	if (some_used)
		vhost_signal(vq->dev, vq);
}

static void vhost_zerocopy_callback(struct ubuf_info *ubuf, bool success)
{
	struct vhost_ubuf_info *vubuf;
	struct vhost_net_ubuf_ref *ubufs;
	struct vhost_virtqueue *vq;
	unsigned long flags;
	int cnt;

	vubuf = container_of(ubuf, struct vhost_ubuf_info, ubuf);
	ubufs = vubuf->ubufs;
	vq = &ubufs->nvq->vq;
	cnt = atomic_read(&ubufs->kref.refcount);

	vubuf->success = success;

	/* FIXME: Is irqsave overkill? bh? */
	spin_lock_irqsave(&ubufs->completed_lock, flags);
	list_add_tail(&vubuf->list, &ubufs->completed);
	spin_unlock_irqrestore(&ubufs->completed_lock, flags);

	/*
	 * Trigger polling thread if guest stopped submitting new buffers:
	 * in this case, the refcount after decrement will eventually reach 1
	 * so here it is 2.
	 * We also trigger polling periodically after each 16 packets
	 * (the value 16 here is more or less arbitrary, it's tuned to trigger
	 * less than 10% of times).
	 */
	if (cnt <= 2 || !(cnt % 16))
		vhost_poll_queue(&vq->poll);
}

/* Expects to be always run from workqueue - which acts as
 * read-size critical section for our kind of RCU. */
static void handle_tx(struct vhost_net *net)
{
	struct vhost_net_virtqueue *nvq = &net->vqs[VHOST_NET_VQ_TX];
	struct vhost_virtqueue *vq = &nvq->vq;
	unsigned out, in, s;
	int head;
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_iov = vq->iov,
		.msg_flags = MSG_DONTWAIT,
	};
	size_t len, total_len = 0;
	int err;
	size_t hdr_size;
	struct socket *sock;
	bool zcopy;

	mutex_lock(&vq->mutex);
	sock = vq->private_data;
	if (!sock)
		goto out;

	/* Do nothing if we are paused. */
	if (net->paused)
		goto out;

	vhost_disable_notify(&net->dev, vq);

	hdr_size = nvq->vhost_hlen;
	zcopy = nvq->ubufs;

	for (;;) {
		struct vhost_ubuf_info *vubuf;

		/* Release DMAs done buffers first */
		if (zcopy)
			vhost_zerocopy_signal_used(net, vq);

		head = vhost_get_vq_desc(&net->dev, vq, vq->iov,
					 ARRAY_SIZE(vq->iov),
					 &out, &in,
					 NULL, NULL);
		/* On error, stop handling until the next kick. */
		if (unlikely(head < 0))
			break;
		/* Nothing new?  Wait for eventfd to tell us they refilled. */
		if (head == vq->num) {
			int num_pends = 0;

			/* If more outstanding DMAs, queue the work. */
			if (nvq->ubufs)
				num_pends = atomic_read(&nvq->ubufs->kref.refcount);
			if (unlikely(num_pends > VHOST_MAX_PEND))
				break;
			if (unlikely(vhost_enable_notify(&net->dev, vq))) {
				vhost_disable_notify(&net->dev, vq);
				continue;
			}
			break;
		}
		if (in) {
			vq_err(vq, "Unexpected descriptor format for TX: "
			       "out %d, int %d\n", out, in);
			break;
		}
		/* Skip header. TODO: support TSO. */
		s = move_iovec_hdr(vq->iov, nvq->hdr, hdr_size, out);
		msg.msg_iovlen = out;
		len = iov_length(vq->iov, out);
		/* Sanity check */
		if (!len) {
			vq_err(vq, "Unexpected header len for TX: "
			       "%zd expected %zd\n",
			       iov_length(nvq->hdr, s), hdr_size);
			break;
		}

		/* We try to allocate vubuf if we want zero copy */
		if (zcopy && len >= VHOST_GOODCOPY_LEN &&
		    vhost_net_tx_select_zcopy(net))
			vubuf = kmem_cache_alloc(ubuf_cache, GFP_KERNEL);
		else
			vubuf = NULL;

		/* use msg_control to pass vhost zerocopy ubuf info to skb */
		if (vubuf) {
			vubuf->id = head;
			vubuf->ubuf.callback = vhost_zerocopy_callback;
			vubuf->ubufs = nvq->ubufs;

			msg.msg_control = vubuf;
			/* Ignored, but fill in for completeness. */
			msg.msg_controllen = sizeof(*vubuf);
			kref_get(&nvq->ubufs->kref);
		} else {
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
		}

		/* TODO: Check specific error and bomb out unless ENOBUFS? */
		err = sock->ops->sendmsg(NULL, sock, &msg, len);
		if (unlikely(err < 0)) {
			if (vubuf) {
				kmem_cache_free(ubuf_cache, vubuf);
				vhost_net_ubuf_put(nvq->ubufs);
			}
			vhost_discard_vq_desc(vq, 1);
			break;
		}
		if (err != len)
			pr_debug("Truncated TX packet: "
				 " len %d != %zd\n", err, len);
		if (!vubuf)
			vhost_add_used_and_signal(&net->dev, vq, head, 0);
		total_len += len;
		vhost_net_tx_packet(net);
		if (unlikely(total_len >= VHOST_NET_WEIGHT)) {
			vhost_poll_queue(&vq->poll);
			break;
		}
	}
out:
	mutex_unlock(&vq->mutex);
}

static int peek_head_len(struct sock *sk)
{
	struct sk_buff *head;
	int len = 0;
	unsigned long flags;

	spin_lock_irqsave(&sk->sk_receive_queue.lock, flags);
	head = skb_peek(&sk->sk_receive_queue);
	if (likely(head)) {
		len = head->len;
		if (vlan_tx_tag_present(head))
			len += VLAN_HLEN;
	}

	spin_unlock_irqrestore(&sk->sk_receive_queue.lock, flags);
	return len;
}

/* This is a multi-buffer version of vhost_get_desc, that works if
 *	vq has read descriptors only.
 * @vq		- the relevant virtqueue
 * @datalen	- data length we'll be reading
 * @iovcount	- returned count of io vectors we fill
 * @log		- vhost log
 * @log_num	- log offset
 * @quota       - headcount quota, 1 for big buffer
 *	returns number of buffer heads allocated, negative on error
 */
static int get_rx_bufs(struct vhost_virtqueue *vq,
		       struct vring_used_elem *heads,
		       int datalen,
		       unsigned *iovcount,
		       struct vhost_log *log,
		       unsigned *log_num,
		       unsigned int quota)
{
	unsigned int out, in;
	int seg = 0;
	int headcount = 0;
	unsigned d;
	int r, nlogs = 0;

	while (datalen > 0 && headcount < quota) {
		if (unlikely(seg >= UIO_MAXIOV)) {
			r = -ENOBUFS;
			goto err;
		}
		d = vhost_get_vq_desc(vq->dev, vq, vq->iov + seg,
				      ARRAY_SIZE(vq->iov) - seg, &out,
				      &in, log, log_num);
		if (d == vq->num) {
			r = 0;
			goto err;
		}
		if (unlikely(out || in <= 0)) {
			vq_err(vq, "unexpected descriptor format for RX: "
				"out %d, in %d\n", out, in);
			r = -EINVAL;
			goto err;
		}
		if (unlikely(log)) {
			nlogs += *log_num;
			log += *log_num;
		}
		heads[headcount].id = d;
		heads[headcount].len = iov_length(vq->iov + seg, in);
		datalen -= heads[headcount].len;
		++headcount;
		seg += in;
	}
	heads[headcount - 1].len += datalen;
	*iovcount = seg;
	if (unlikely(log))
		*log_num = nlogs;
	return headcount;
err:
	vhost_discard_vq_desc(vq, headcount);
	return r;
}

/* Expects to be always run from workqueue - which acts as
 * read-size critical section for our kind of RCU. */
static void handle_rx(struct vhost_net *net)
{
	struct vhost_net_virtqueue *nvq = &net->vqs[VHOST_NET_VQ_RX];
	struct vhost_virtqueue *vq = &nvq->vq;
	unsigned uninitialized_var(in), log;
	struct vhost_log *vq_log;
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_control = NULL, /* FIXME: get and handle RX aux data. */
		.msg_controllen = 0,
		.msg_iov = vq->iov,
		.msg_flags = MSG_DONTWAIT,
	};
	struct virtio_net_hdr_mrg_rxbuf hdr = {
		.hdr.flags = 0,
		.hdr.gso_type = VIRTIO_NET_HDR_GSO_NONE
	};
	size_t total_len = 0;
	int err, mergeable;
	s16 headcount;
	size_t vhost_hlen, sock_hlen;
	size_t vhost_len, sock_len;
	struct socket *sock;

	mutex_lock(&vq->mutex);
	sock = vq->private_data;
	if (!sock)
		goto out;
	/* Do nothing if we are paused. */
	if (net->paused)
		goto out;

	vhost_disable_notify(&net->dev, vq);

	vhost_hlen = nvq->vhost_hlen;
	sock_hlen = nvq->sock_hlen;

	vq_log = unlikely(vhost_has_feature(&net->dev, VHOST_F_LOG_ALL)) ?
		vq->log : NULL;
	mergeable = vhost_has_feature(&net->dev, VIRTIO_NET_F_MRG_RXBUF);

	while ((sock_len = peek_head_len(sock->sk))) {
		sock_len += sock_hlen;
		vhost_len = sock_len + vhost_hlen;
		headcount = get_rx_bufs(vq, vq->heads, vhost_len,
					&in, vq_log, &log,
					likely(mergeable) ? UIO_MAXIOV : 1);
		/* On error, stop handling until the next kick. */
		if (unlikely(headcount < 0))
			break;
		/* OK, now we need to know about added descriptors. */
		if (!headcount) {
			if (unlikely(vhost_enable_notify(&net->dev, vq))) {
				/* They have slipped one in as we were
				 * doing that: check again. */
				vhost_disable_notify(&net->dev, vq);
				continue;
			}
			/* Nothing new?  Wait for eventfd to tell us
			 * they refilled. */
			break;
		}
		/* We don't need to be notified again. */
		if (unlikely((vhost_hlen)))
			/* Skip header. TODO: support TSO. */
			move_iovec_hdr(vq->iov, nvq->hdr, vhost_hlen, in);
		else
			/* Copy the header for use in VIRTIO_NET_F_MRG_RXBUF:
			 * needed because recvmsg can modify msg_iov. */
			copy_iovec_hdr(vq->iov, nvq->hdr, sock_hlen, in);
		msg.msg_iovlen = in;
		err = sock->ops->recvmsg(NULL, sock, &msg,
					 sock_len, MSG_DONTWAIT | MSG_TRUNC);
		/* Userspace might have consumed the packet meanwhile:
		 * it's not supposed to do this usually, but might be hard
		 * to prevent. Discard data we got (if any) and keep going. */
		if (unlikely(err != sock_len)) {
			pr_debug("Discarded rx packet: "
				 " len %d, expected %zd\n", err, sock_len);
			vhost_discard_vq_desc(vq, headcount);
			continue;
		}
		if (unlikely(vhost_hlen) &&
		    memcpy_toiovecend(nvq->hdr, (unsigned char *)&hdr, 0,
				      vhost_hlen)) {
			vq_err(vq, "Unable to write vnet_hdr at addr %p\n",
			       vq->iov->iov_base);
			break;
		}
		/* TODO: Should check and handle checksum. */
		if (likely(mergeable) &&
		    memcpy_toiovecend(nvq->hdr, (unsigned char *)&headcount,
				      offsetof(typeof(hdr), num_buffers),
				      sizeof hdr.num_buffers)) {
			vq_err(vq, "Failed num_buffers write");
			vhost_discard_vq_desc(vq, headcount);
			break;
		}
		vhost_add_used_and_signal_n(&net->dev, vq, vq->heads,
					    headcount);
		if (unlikely(vq_log))
			vhost_log_write(vq, vq_log, log, vhost_len);
		total_len += vhost_len;
		if (unlikely(total_len >= VHOST_NET_WEIGHT)) {
			vhost_poll_queue(&vq->poll);
			break;
		}
	}
out:
	mutex_unlock(&vq->mutex);
}

static void handle_tx_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_net *net = container_of(vq->dev, struct vhost_net, dev);

	handle_tx(net);
}

static void handle_rx_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_net *net = container_of(vq->dev, struct vhost_net, dev);

	handle_rx(net);
}

static void handle_tx_net(struct vhost_work *work)
{
	struct vhost_net *net = container_of(work, struct vhost_net,
					     poll[VHOST_NET_VQ_TX].work);
	handle_tx(net);
}

static void handle_rx_net(struct vhost_work *work)
{
	struct vhost_net *net = container_of(work, struct vhost_net,
					     poll[VHOST_NET_VQ_RX].work);
	handle_rx(net);
}

static int vhost_net_open(struct inode *inode, struct file *f)
{
	struct vhost_net *n = kmalloc(sizeof *n, GFP_KERNEL);
	struct vhost_dev *dev;
	struct vhost_virtqueue **vqs;
	int r, i;

	if (!n)
		return -ENOMEM;
	vqs = kmalloc(VHOST_NET_VQ_MAX * sizeof(*vqs), GFP_KERNEL);
	if (!vqs) {
		kfree(n);
		return -ENOMEM;
	}

	dev = &n->dev;
	vqs[VHOST_NET_VQ_TX] = &n->vqs[VHOST_NET_VQ_TX].vq;
	vqs[VHOST_NET_VQ_RX] = &n->vqs[VHOST_NET_VQ_RX].vq;
	n->vqs[VHOST_NET_VQ_TX].vq.handle_kick = handle_tx_kick;
	n->vqs[VHOST_NET_VQ_RX].vq.handle_kick = handle_rx_kick;
	for (i = 0; i < VHOST_NET_VQ_MAX; i++) {
		n->vqs[i].ubufs = NULL;
		n->vqs[i].vhost_hlen = 0;
		n->vqs[i].sock_hlen = 0;
	}
	r = vhost_dev_init(dev, vqs, VHOST_NET_VQ_MAX);
	if (r < 0) {
		kfree(n);
		kfree(vqs);
		return r;
	}

	vhost_poll_init(n->poll + VHOST_NET_VQ_TX, handle_tx_net, POLLOUT, dev);
	vhost_poll_init(n->poll + VHOST_NET_VQ_RX, handle_rx_net, POLLIN, dev);
	n->paused = 0;

	f->private_data = n;

	return 0;
}

static void vhost_net_disable_vq(struct vhost_net *n,
				 struct vhost_virtqueue *vq)
{
	struct vhost_net_virtqueue *nvq =
		container_of(vq, struct vhost_net_virtqueue, vq);
	struct vhost_poll *poll = n->poll + (nvq - n->vqs);
	if (!vq->private_data)
		return;
	vhost_poll_stop(poll);
}

static int vhost_net_enable_vq(struct vhost_net *n,
				struct vhost_virtqueue *vq)
{
	struct vhost_net_virtqueue *nvq =
		container_of(vq, struct vhost_net_virtqueue, vq);
	struct vhost_poll *poll = n->poll + (nvq - n->vqs);
	struct socket *sock;

	sock = vq->private_data;
	if (!sock)
		return 0;

	return vhost_poll_start(poll, sock->file);
}

static struct socket *vhost_net_stop_vq(struct vhost_net *n,
					struct vhost_virtqueue *vq)
{
	struct socket *sock;

	mutex_lock(&vq->mutex);
	sock = vq->private_data;
	vhost_net_disable_vq(n, vq);
	vq->private_data = NULL;
	mutex_unlock(&vq->mutex);
	return sock;
}

static void vhost_net_stop(struct vhost_net *n, struct socket **tx_sock,
			   struct socket **rx_sock)
{
	*tx_sock = vhost_net_stop_vq(n, &n->vqs[VHOST_NET_VQ_TX].vq);
	*rx_sock = vhost_net_stop_vq(n, &n->vqs[VHOST_NET_VQ_RX].vq);
}

/* Should be holding n->dev.mutex, though ->release doesn't need to */
static void __vhost_net_pause(struct vhost_net *n)
{
	unsigned int i;

	n->paused++;
	if (n->paused > 1)
		return;

	/* Wait for any pending zero copy buffers. */
	for (i = 0; i < VHOST_NET_VQ_MAX; i++) {
		struct vhost_net_virtqueue *vq = &n->vqs[i];

		if (!vq->ubufs)
			continue;

		/* Drop refcount so they call wakeup callback. */
		atomic_dec(&vq->ubufs->kref.refcount);

		/* Make sure it's seen new paused value */
		mutex_lock(&vq->vq.mutex);
		mutex_unlock(&vq->vq.mutex);

		wait_event(vq->ubufs->wait,
			   !atomic_read(&vq->ubufs->kref.refcount));

		/* Restore reference count. */
		atomic_inc(&vq->ubufs->kref.refcount);
	}
}

static void __vhost_net_unpause(struct vhost_net *n)
{
	unsigned int i;

	n->paused--;
	if (!n->paused) {
		for (i = 0; i < VHOST_NET_VQ_MAX; i++)
			vhost_poll_queue(&n->vqs[i].vq.poll);
	}
}

/* Stop the device, so we can play with it. */
static void vhost_net_pause(struct vhost_net *n)
{
	mutex_lock(&n->dev.mutex);
	__vhost_net_pause(n);
	mutex_unlock(&n->dev.mutex);
}

static void vhost_net_unpause(struct vhost_net *n)
{
	mutex_lock(&n->dev.mutex);
	__vhost_net_unpause(n);
	mutex_unlock(&n->dev.mutex);
}

static void vhost_net_flush_vq(struct vhost_net *n, int index)
{
	vhost_poll_flush(n->poll + index);
	vhost_poll_flush(&n->vqs[index].vq.poll);
}

static void vhost_net_flush(struct vhost_net *n)
{
	vhost_net_flush_vq(n, VHOST_NET_VQ_TX);
	vhost_net_flush_vq(n, VHOST_NET_VQ_RX);
}

static int vhost_net_release(struct inode *inode, struct file *f)
{
	struct vhost_net *n = f->private_data;
	struct socket *tx_sock;
	struct socket *rx_sock;

	vhost_net_stop(n, &tx_sock, &rx_sock);
	vhost_net_pause(n);
	vhost_net_flush(n);
	vhost_dev_stop(&n->dev);
	vhost_dev_cleanup(&n->dev, false);
	vhost_net_vq_reset(n);
	if (tx_sock)
		fput(tx_sock->file);
	if (rx_sock)
		fput(rx_sock->file);
	kfree(n->dev.vqs);
	kfree(n);
	return 0;
}

static struct socket *get_raw_socket(int fd)
{
	struct {
		struct sockaddr_ll sa;
		char  buf[MAX_ADDR_LEN];
	} uaddr;
	int uaddr_len = sizeof uaddr, r;
	struct socket *sock = sockfd_lookup(fd, &r);

	if (!sock)
		return ERR_PTR(-ENOTSOCK);

	/* Parameter checking */
	if (sock->sk->sk_type != SOCK_RAW) {
		r = -ESOCKTNOSUPPORT;
		goto err;
	}

	r = sock->ops->getname(sock, (struct sockaddr *)&uaddr.sa,
			       &uaddr_len, 0);
	if (r)
		goto err;

	if (uaddr.sa.sll_family != AF_PACKET) {
		r = -EPFNOSUPPORT;
		goto err;
	}
	return sock;
err:
	fput(sock->file);
	return ERR_PTR(r);
}

static struct socket *get_tap_socket(int fd)
{
	struct file *file = fget(fd);
	struct socket *sock;

	if (!file)
		return ERR_PTR(-EBADF);
	sock = tun_get_socket(file);
	if (!IS_ERR(sock))
		return sock;
	sock = macvtap_get_socket(file);
	if (IS_ERR(sock))
		fput(file);
	return sock;
}

static struct socket *get_socket(int fd)
{
	struct socket *sock;

	/* special case to disable backend */
	if (fd == -1)
		return NULL;
	sock = get_raw_socket(fd);
	if (!IS_ERR(sock))
		return sock;
	sock = get_tap_socket(fd);
	if (!IS_ERR(sock))
		return sock;
	return ERR_PTR(-ENOTSOCK);
}

static long vhost_net_set_backend(struct vhost_net *n, unsigned index, int fd)
{
	struct socket *sock, *oldsock;
	struct vhost_virtqueue *vq;
	struct vhost_net_virtqueue *nvq;
	struct vhost_net_ubuf_ref *ubufs, *oldubufs = NULL;
	int r;

	mutex_lock(&n->dev.mutex);
	r = vhost_dev_check_owner(&n->dev);
	if (r)
		goto err;

	if (index >= VHOST_NET_VQ_MAX) {
		r = -ENOBUFS;
		goto err;
	}
	vq = &n->vqs[index].vq;
	nvq = &n->vqs[index];
	mutex_lock(&vq->mutex);

	/* Verify that ring has been setup correctly. */
	if (!vhost_vq_access_ok(vq)) {
		r = -EFAULT;
		goto err_vq;
	}
	sock = get_socket(fd);
	if (IS_ERR(sock)) {
		r = PTR_ERR(sock);
		goto err_vq;
	}

	/* start polling new socket */
	oldsock = vq->private_data;
	if (sock != oldsock) {
		ubufs = vhost_net_ubuf_alloc(nvq,
					     sock && vhost_sock_zcopy(sock));
		if (IS_ERR(ubufs)) {
			r = PTR_ERR(ubufs);
			goto err_ubufs;
		}

		vhost_net_disable_vq(n, vq);
		vq->private_data = sock;
		r = vhost_init_used(vq);
		if (r)
			goto err_used;
		r = vhost_net_enable_vq(n, vq);
		if (r)
			goto err_used;

		oldubufs = nvq->ubufs;
		nvq->ubufs = ubufs;

		n->tx_packets = 0;
		n->tx_zcopy_err = 0;
	}

	mutex_unlock(&vq->mutex);

	if (oldubufs)
		vhost_net_ubuf_put_wait_and_free(oldubufs);

	if (oldsock) {
		vhost_net_flush_vq(n, index);
		fput(oldsock->file);
	}

	mutex_unlock(&n->dev.mutex);
	return 0;

err_used:
	vq->private_data = oldsock;
	vhost_net_enable_vq(n, vq);
	if (ubufs)
		vhost_net_ubuf_put_wait_and_free(ubufs);
err_ubufs:
	fput(sock->file);
err_vq:
	mutex_unlock(&vq->mutex);
err:
	mutex_unlock(&n->dev.mutex);
	return r;
}

static long vhost_net_reset_owner(struct vhost_net *n)
{
	struct socket *tx_sock = NULL;
	struct socket *rx_sock = NULL;
	long err;
	struct vhost_memory *memory;

	mutex_lock(&n->dev.mutex);
	err = vhost_dev_check_owner(&n->dev);
	if (err)
		goto done;
	memory = vhost_dev_reset_owner_prepare();
	if (!memory) {
		err = -ENOMEM;
		goto done;
	}
	vhost_net_stop(n, &tx_sock, &rx_sock);
	__vhost_net_pause(n);
	vhost_net_flush(n);
	vhost_dev_reset_owner(&n->dev, memory);
	vhost_net_vq_reset(n);
	__vhost_net_unpause(n);
done:
	mutex_unlock(&n->dev.mutex);
	if (tx_sock)
		fput(tx_sock->file);
	if (rx_sock)
		fput(rx_sock->file);
	return err;
}

static int vhost_net_set_features(struct vhost_net *n, u64 features)
{
	size_t vhost_hlen, sock_hlen, hdr_len;
	int i;

	hdr_len = (features & (1 << VIRTIO_NET_F_MRG_RXBUF)) ?
			sizeof(struct virtio_net_hdr_mrg_rxbuf) :
			sizeof(struct virtio_net_hdr);
	if (features & (1 << VHOST_NET_F_VIRTIO_NET_HDR)) {
		/* vhost provides vnet_hdr */
		vhost_hlen = hdr_len;
		sock_hlen = 0;
	} else {
		/* socket provides vnet_hdr */
		vhost_hlen = 0;
		sock_hlen = hdr_len;
	}
	mutex_lock(&n->dev.mutex);
	if ((features & (1 << VHOST_F_LOG_ALL)) &&
	    !vhost_log_access_ok(&n->dev)) {
		mutex_unlock(&n->dev.mutex);
		return -EFAULT;
	}
	n->dev.acked_features = features;
	smp_wmb();
	for (i = 0; i < VHOST_NET_VQ_MAX; ++i) {
		mutex_lock(&n->vqs[i].vq.mutex);
		n->vqs[i].vhost_hlen = vhost_hlen;
		n->vqs[i].sock_hlen = sock_hlen;
		mutex_unlock(&n->vqs[i].vq.mutex);
	}
	vhost_net_flush(n);
	mutex_unlock(&n->dev.mutex);
	return 0;
}

static long vhost_net_set_owner(struct vhost_net *n)
{
	int r;

	mutex_lock(&n->dev.mutex);
	if (vhost_dev_has_owner(&n->dev)) {
		r = -EBUSY;
		goto out;
	}
	r = vhost_dev_set_owner(&n->dev);
	vhost_net_flush(n);
out:
	mutex_unlock(&n->dev.mutex);
	return r;
}

static long vhost_net_ioctl(struct file *f, unsigned int ioctl,
			    unsigned long arg)
{
	struct vhost_net *n = f->private_data;
	void __user *argp = (void __user *)arg;
	u64 __user *featurep = argp;
	struct vhost_vring_file backend;
	u64 features;
	int r;

	vhost_net_pause(n);

	switch (ioctl) {
	case VHOST_NET_SET_BACKEND:
		if (copy_from_user(&backend, argp, sizeof backend))
			r = -EFAULT;
		else
			r = vhost_net_set_backend(n, backend.index, backend.fd);
		break;
	case VHOST_GET_FEATURES:
		features = VHOST_NET_FEATURES;
		if (copy_to_user(featurep, &features, sizeof features))
			r = -EFAULT;
		else
			r = 0;
		break;
	case VHOST_SET_FEATURES:
		if (copy_from_user(&features, featurep, sizeof features))
			r = -EFAULT;
		else if (features & ~VHOST_NET_FEATURES)
			r = -EOPNOTSUPP;
		else
			r = vhost_net_set_features(n, features);
		break;
	case VHOST_RESET_OWNER:
		r = vhost_net_reset_owner(n);
		break;
	case VHOST_SET_OWNER:
		r = vhost_net_set_owner(n);
		break;
	default:
		mutex_lock(&n->dev.mutex);
		r = vhost_dev_ioctl(&n->dev, ioctl, argp);
		if (r == -ENOIOCTLCMD)
			r = vhost_vring_ioctl(&n->dev, ioctl, argp);
		else
			vhost_net_flush(n);
		mutex_unlock(&n->dev.mutex);
	}

	vhost_net_unpause(n);
	return r;
}

#ifdef CONFIG_COMPAT
static long vhost_net_compat_ioctl(struct file *f, unsigned int ioctl,
				   unsigned long arg)
{
	return vhost_net_ioctl(f, ioctl, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations vhost_net_fops = {
	.owner          = THIS_MODULE,
	.release        = vhost_net_release,
	.unlocked_ioctl = vhost_net_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = vhost_net_compat_ioctl,
#endif
	.open           = vhost_net_open,
	.llseek		= noop_llseek,
};

static struct miscdevice vhost_net_misc = {
	.minor = VHOST_NET_MINOR,
	.name = "vhost-net",
	.fops = &vhost_net_fops,
};

static int vhost_net_init(void)
{
	int ret;

	if (experimental_zcopytx)
		vhost_net_enable_zcopy(VHOST_NET_VQ_TX);
	ubuf_cache = KMEM_CACHE(vhost_ubuf_info, 0);
	if (!ubuf_cache)
		return -ENOMEM;
	ret = misc_register(&vhost_net_misc);
	if (ret != 0)
		kmem_cache_destroy(ubuf_cache);
	return ret;
}
module_init(vhost_net_init);

static void vhost_net_exit(void)
{
	misc_deregister(&vhost_net_misc);
	kmem_cache_destroy(ubuf_cache);
}
module_exit(vhost_net_exit);

MODULE_VERSION("0.0.1");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Michael S. Tsirkin");
MODULE_DESCRIPTION("Host kernel accelerator for virtio net");
MODULE_ALIAS_MISCDEV(VHOST_NET_MINOR);
MODULE_ALIAS("devname:vhost-net");
