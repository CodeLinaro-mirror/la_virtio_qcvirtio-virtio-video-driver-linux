/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *    Gerd Hoffmann <kraxel@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "virtio_video.h"

#define MAX_INLINE_CMD_SIZE 298
#define VBUFFER_SIZE (sizeof(struct virtio_video_vbuffer) + MAX_INLINE_CMD_SIZE)

#define EVENT_BUFFER_SIZE 256

struct virtio_video_vbuffer {
	refcount_t refcnt;

	void *buf;
	size_t size;

	void *data_buf;
	size_t data_size;

	void *resp_buf;
	size_t resp_size;

	struct completion reclaimed;
};

struct virtio_video_async_response_entry;

typedef void (*virtio_video_async_resp_cb)(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags);

struct virtio_video_async_response_entry {
	refcount_t refcnt;

	uint32_t type;
	uint32_t cookie;
	struct virtio_video_stream *stream;
	struct virtio_video_buffer *buf;

	virtio_video_async_resp_cb cb;
	struct completion reclaimed;

	struct list_head stream_list;
};

static int virtio_video_queue_event_buffer(struct virtio_video_device *vvd,
					   void *buf, unsigned int len);
static void virtio_video_handle_event(struct virtio_video_device *vvd,
				      void *buf, unsigned int len);

static void virtio_video_handle_async_response_get_params(
	struct virtio_video_device *vvd, struct virtio_video_stream *stream,
	void *buf, unsigned int len, uint32_t event_flags);
static void virtio_video_handle_async_response_queue(
	struct virtio_video_device *vvd, struct virtio_video_stream *stream,
	struct virtio_video_resource_queue_async_resp *evt,
	struct virtio_video_async_response_entry *entry, uint32_t event_flags);

void virtio_video_stream_id_get(struct virtio_video_device *vvd,
				struct virtio_video_stream *stream,
				uint32_t *id)
{
	int handle;

	mutex_lock(&vvd->stream_idr_lock);
	handle = idr_alloc(&vvd->stream_idr, stream, 1, 0, GFP_KERNEL);
	mutex_unlock(&vvd->stream_idr_lock);
	*id = handle;
}

void virtio_video_stream_id_put(struct virtio_video_device *vvd, uint32_t id)
{
	mutex_lock(&vvd->stream_idr_lock);
	idr_remove(&vvd->stream_idr, id);
	mutex_unlock(&vvd->stream_idr_lock);
}

void virtio_video_resource_id_get(struct virtio_video_device *vvd, uint32_t *id)
{
	int handle;

	idr_preload(GFP_KERNEL);
	spin_lock(&vvd->resource_idr_lock);
	handle = idr_alloc(&vvd->resource_idr, NULL, 1, 0, GFP_ATOMIC);
	spin_unlock(&vvd->resource_idr_lock);
	idr_preload_end();
	*id = handle;
}

void virtio_video_resource_id_put(struct virtio_video_device *vvd, uint32_t id)
{
	spin_lock(&vvd->resource_idr_lock);
	idr_remove(&vvd->resource_idr, id);
	spin_unlock(&vvd->resource_idr_lock);
}

void virtio_video_release_vbuf(struct virtio_video_device *vvd,
			       struct virtio_video_vbuffer *vbuf)
{
	if (refcount_dec_and_test(&vbuf->refcnt)) {
		kfree(vbuf->data_buf);
		kfree(vbuf->resp_buf);
		kmem_cache_free(vvd->vbufs, vbuf);
	}
}

void virtio_video_cmd_cb(struct virtqueue *vq)
{
	struct virtio_video_device *vvd = vq->vdev->priv;
	struct virtio_video_vbuffer *vbuf;
	unsigned long flags;
	unsigned int len;
	int reclaimed = 0;

	spin_lock_irqsave(&vvd->commandq.qlock, flags);
	do {
		virtqueue_disable_cb(vq);

		while ((vbuf = virtqueue_get_buf(vq, &len))) {
			complete(&vbuf->reclaimed);
			virtio_video_release_vbuf(vvd, vbuf);
			reclaimed++;
		}
	} while (!virtqueue_enable_cb(vq));
	spin_unlock_irqrestore(&vvd->commandq.qlock, flags);

	if (reclaimed == 0)
		v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
			 "zero vbufs reclaimed\n");

	wake_up(&vvd->commandq.reclaim_queue);
}

void virtio_video_process_events(struct work_struct *work)
{
	struct virtio_video_device *vvd =
		container_of(work, struct virtio_video_device, eventq.work);
	struct virtqueue *vq = vvd->eventq.vq;
	void *buf;
	unsigned int len;
	int reclaimed = 0;

	do {
		virtqueue_disable_cb(vq);

		while ((buf = virtqueue_get_buf(vq, &len))) {
			virtio_video_handle_event(vvd, buf, len);
			virtio_video_queue_event_buffer(vvd, buf,
							EVENT_BUFFER_SIZE);
			reclaimed++;
		}
	} while (!virtqueue_enable_cb(vq));

	if (reclaimed == 0)
		v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
			 "zero vbufs reclaimed\n");
	else
		virtqueue_kick(vq);
}

void virtio_video_event_cb(struct virtqueue *vq)
{
	struct virtio_video_device *vvd = vq->vdev->priv;

	schedule_work(&vvd->eventq.work);
}

/* Takes ownership of resp_buf, therefore has to kfree it in case of error. */
static struct virtio_video_vbuffer *
virtio_video_get_vbuf(struct virtio_video_device *vvd, size_t size,
		      size_t resp_size, void *resp_buf)
{
	struct virtio_video_vbuffer *vbuf;

	vbuf = kmem_cache_zalloc(vvd->vbufs, GFP_KERNEL);
	if (!vbuf) {
		v4l2_err(&vvd->v4l2_dev, "failed to allocate a vbuffer\n");
		kfree(resp_buf);
		return ERR_PTR(-ENOMEM);
	}

	refcount_set(&vbuf->refcnt, 1);

	BUG_ON(size > MAX_INLINE_CMD_SIZE);
	vbuf->buf = (void *)vbuf + sizeof(*vbuf);
	vbuf->size = size;

	vbuf->resp_size = resp_size;
	vbuf->resp_buf = resp_buf;

	init_completion(&vbuf->reclaimed);

	return vbuf;
}

int virtio_video_alloc_vbufs(struct virtio_video_device *vvd)
{
	vvd->vbufs = kmem_cache_create("virtio-video-vbufs", VBUFFER_SIZE,
				       __alignof__(struct virtio_video_vbuffer),
				       0, NULL);
	if (!vvd->vbufs)
		return -ENOMEM;

	return 0;
}

void virtio_video_free_vbufs(struct virtio_video_device *vvd)
{
	struct virtio_video_vbuffer *vbuf;

	/* Release command buffers. Operation on vbufs here is safe, because by
	 * this time the device was reset and virtqueues were stopped.
	 */
	while ((vbuf = virtqueue_detach_unused_buf(vvd->commandq.vq))) {
		/* Unblock waiters */
		complete(&vbuf->reclaimed);
		virtio_video_release_vbuf(vvd, vbuf);
	}

	wake_up(&vvd->commandq.reclaim_queue);

	/* TODO: have to wait for all vbufs to be freed first */
	kmem_cache_destroy(vvd->vbufs);
	vvd->vbufs = NULL;

	/* Release event buffers */
	while (virtqueue_detach_unused_buf(vvd->eventq.vq))
		;

	kfree(vvd->events_buf);
	vvd->events_buf = NULL;
}

int virtio_video_alloc_async_responses(struct virtio_video_device *vvd)
{
	vvd->async_resp_bufs = kmem_cache_create(
		"virtio-video-async-resp-bufs",
		sizeof(struct virtio_video_async_response_entry),
		__alignof__(struct virtio_video_async_response_entry), 0, NULL);
	if (!vvd->async_resp_bufs)
		return -ENOMEM;

	idr_init(&vvd->async_resp_cookie_idr);
	spin_lock_init(&vvd->async_resp_lock);

	return 0;
}

void virtio_video_free_async_responses(struct virtio_video_device *vvd)
{
	unsigned long flags;
	//struct virtio_video_async_response_entry *entry;

	spin_lock_irqsave(&vvd->async_resp_lock, flags);
	WARN_ON(!idr_is_empty(&vvd->async_resp_cookie_idr));
	//idr_for_each_entry_ul() {}
	spin_unlock_irqrestore(&vvd->async_resp_lock, flags);

	/* TODO: have to wake up waiters and wait for all elements to be freed first? */
	kmem_cache_destroy(vvd->async_resp_bufs);
	vvd->async_resp_bufs = NULL;
}

static struct virtio_video_async_response_entry *
alloc_async_response(struct virtio_video_device *vvd)
{
	struct virtio_video_async_response_entry *entry;

	entry = kmem_cache_zalloc(vvd->async_resp_bufs, GFP_KERNEL);
	if (!entry) {
		v4l2_err(&vvd->v4l2_dev,
			 "failed to allocate an async response entry\n");
		return ERR_PTR(-ENOMEM);
	}

	refcount_set(&entry->refcnt, 1);
	init_completion(&entry->reclaimed);
	INIT_LIST_HEAD(&entry->stream_list);

	return entry;
}

static inline void
free_unused_async_response(struct virtio_video_device *vvd,
			   struct virtio_video_async_response_entry *entry)
{
	kmem_cache_free(vvd->async_resp_bufs, entry);
}

/* must be called with vvd->async_resp_lock held. */
static bool virtio_video_release_async_response(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry **entry)
{
	if (refcount_dec_and_test(&(*entry)->refcnt)) {
		(*entry)->stream = NULL;
		(*entry)->buf = NULL;
		list_del_init(&(*entry)->stream_list);
		idr_remove(&vvd->async_resp_cookie_idr, (*entry)->cookie);
		free_unused_async_response(vvd, *entry);
		*entry = NULL;
		return true;
	}

	return false;
}

void virtio_video_cleanup_stream_async_responses(
	struct virtio_video_stream *stream)
{
	struct virtio_video_device *vvd = to_virtio_vd(stream->video_dev);
	unsigned long flags;
	struct virtio_video_async_response_entry *entry, *next;
	int num_released = 0;

	spin_lock_irqsave(&vvd->async_resp_lock, flags);
	list_for_each_entry_safe(entry, next, &stream->async_response_list,
				 stream_list) {
		v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
			 "stream_id=%u: free async resp entry cookie %u\n",
			 stream->stream_id, entry->cookie);
		/* Release the device's reference. It is safe to assume, that the
		 * device won't send any more async responses for this stream because
		 * this function is called after STREAM_CLOSE command has finished.
		 */
		if (!virtio_video_release_async_response(vvd, &entry))
			/* Delete the entry from the list anyway because the list head is
			 * going to be freed.
			 */
			list_del_init(&entry->stream_list);
		num_released++;
	}
	spin_unlock_irqrestore(&vvd->async_resp_lock, flags);

	if (num_released > 1)
		pr_warn("%s released too many entries: %d\n", __func__,
			num_released);
}

/* Takes ownership of resp_buf, therefore has to kfree it in case of error. */
static void *
virtio_video_alloc_req_resp(struct virtio_video_device *vvd,
			    struct virtio_video_vbuffer **vbuffer_p,
			    size_t size, size_t resp_size, void *resp_buf)
{
	struct virtio_video_vbuffer *vbuf;

	vbuf = virtio_video_get_vbuf(vvd, size, resp_size, resp_buf);
	if (IS_ERR(vbuf)) {
		*vbuffer_p = NULL;
		return ERR_CAST(vbuf);
	}
	*vbuffer_p = vbuf;

	return vbuf->buf;
}

static inline void *
virtio_video_alloc_req(struct virtio_video_device *vvd,
		       struct virtio_video_vbuffer **vbuffer_p, size_t size)
{
	return virtio_video_alloc_req_resp(vvd, vbuffer_p, size, 0, NULL);
}

static void *virtio_video_alloc_stream_cmd_async_resp(
	struct virtio_video_device *vvd,
	struct virtio_video_vbuffer **vbuffer_p, size_t size,
	struct virtio_video_async_response_entry **entry_p,
	struct virtio_video_stream *stream)
{
	struct virtio_video_cmd_hdr *hdr;
	struct virtio_video_async_response_entry *entry;
	unsigned long flags;
	int ret;

	hdr = virtio_video_alloc_req(vvd, vbuffer_p, size);
	if (IS_ERR(hdr))
		return hdr;

	entry = alloc_async_response(vvd);
	if (IS_ERR(entry)) {
		hdr = ERR_CAST(entry);
		goto err_free_vbuf;
	}

	entry->cookie = 1; /* start id for idr */

	idr_preload(GFP_KERNEL);
	spin_lock_irqsave(&vvd->async_resp_lock, flags);
	ret = idr_alloc_u32(&vvd->async_resp_cookie_idr, entry, &entry->cookie,
			    UINT_MAX, GFP_ATOMIC);
	if (ret != 0) {
		spin_unlock_irqrestore(&vvd->async_resp_lock, flags);
		idr_preload_end();
		v4l2_err(&vvd->v4l2_dev, "idr allocation failed\n");
		goto err_free_entry;
	}
	list_add(&entry->stream_list, &stream->async_response_list);
	spin_unlock_irqrestore(&vvd->async_resp_lock, flags);
	idr_preload_end();

	entry->stream = stream;
	hdr->async_response_cookie = cpu_to_le32(entry->cookie);
	*entry_p = entry;

	return hdr;

err_free_entry:
	free_unused_async_response(vvd, entry);
err_free_vbuf:
	virtio_video_release_vbuf(vvd, *vbuffer_p);
	return hdr;
}

/* Consumes vbuf, therefore doesn't increase the refcount */
static int virtio_video_queue_cmd_buffer(struct virtio_video_device *vvd,
					 struct virtio_video_vbuffer *vbuf)
{
	unsigned long flags;
	struct virtqueue *vq = vvd->commandq.vq;
	struct scatterlist *sgs[3], vreq, vout, vresp;
	int outcnt = 0, incnt = 0;
	int ret;

	sg_init_one(&vreq, vbuf->buf, vbuf->size);
	sgs[outcnt + incnt] = &vreq;
	outcnt++;

	if (vbuf->data_size) {
		sg_init_one(&vout, vbuf->data_buf, vbuf->data_size);
		sgs[outcnt + incnt] = &vout;
		outcnt++;
	}

	if (vbuf->resp_size) {
		sg_init_one(&vresp, vbuf->resp_buf, vbuf->resp_size);
		sgs[outcnt + incnt] = &vresp;
		incnt++;
	}

	spin_lock_irqsave(&vvd->commandq.qlock, flags);

retry:
	ret = virtqueue_add_sgs(vq, sgs, outcnt, incnt, vbuf, GFP_ATOMIC);
	if (ret == -ENOSPC) {
		spin_unlock_irqrestore(&vvd->commandq.qlock, flags);
		wait_event(vvd->commandq.reclaim_queue, vq->num_free);
		spin_lock_irqsave(&vvd->commandq.qlock, flags);
		goto retry;
	} else if (!ret)
		virtqueue_kick(vq);

	spin_unlock_irqrestore(&vvd->commandq.qlock, flags);

	if (ret)
		virtio_video_release_vbuf(vvd, vbuf);

	return ret;
}

/* Consumes vbuf except when returns 0 && keep_vbuf == true */
static int virtio_video_queue_cmd_buffer_sync(struct virtio_video_device *vvd,
					      struct virtio_video_vbuffer *vbuf,
					      bool keep_vbuf)
{
	int ret;
	unsigned long rem;

	/* Keep our copy for waiting and
	 * for response handling when returning 0 && keep_vbuf == true.
	 */
	refcount_inc(&vbuf->refcnt);

	ret = virtio_video_queue_cmd_buffer(vvd, vbuf);
	if (ret)
		goto out;

	rem = wait_for_completion_timeout(&vbuf->reclaimed, 5 * HZ);
	if (rem == 0) {
		v4l2_err(&vvd->v4l2_dev, "timed out waiting for a response\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	if (keep_vbuf)
		return ret;

out:
	virtio_video_release_vbuf(vvd, vbuf);
	return ret;
}

/* Consumes entry except when returns 0 && keep_entry == true */
static int virtio_video_queue_cmd_buffer_wait_async_resp(
	struct virtio_video_device *vvd, struct virtio_video_vbuffer *vbuf,
	struct virtio_video_async_response_entry *entry)
{
	int ret;
	unsigned long rem;
	unsigned long flags;

	/* Keep our copy for waiting and
	 * for response handling when returning 0 && keep_entry == true.
	 */
	refcount_inc(&entry->refcnt);

	ret = virtio_video_queue_cmd_buffer(vvd, vbuf);
	if (ret)
		goto out;

	rem = wait_for_completion_timeout(&entry->reclaimed, 5 * HZ);
	if (rem == 0) {
		v4l2_err(&vvd->v4l2_dev,
			 "timed out waiting for an async response type %u\n",
			 entry->type);
		ret = -ETIMEDOUT;
	}

out:
	/* Release this function's reference. */
	spin_lock_irqsave(&vvd->async_resp_lock, flags);
	virtio_video_release_async_response(vvd, &entry);
	spin_unlock_irqrestore(&vvd->async_resp_lock, flags);
	return ret;
}

static int virtio_video_queue_event_buffer(struct virtio_video_device *vvd,
					   void *buf, unsigned int len)
{
	int ret;
	struct scatterlist sg;
	struct virtqueue *vq = vvd->eventq.vq;

	memset(buf, 0, len);
	sg_init_one(&sg, buf, len);

	ret = virtqueue_add_inbuf(vq, &sg, 1, buf, GFP_KERNEL);
	if (ret) {
		v4l2_err(&vvd->v4l2_dev, "failed to queue event buffer\n");
		return ret;
	}

	return 0;
}

static void virtio_video_handle_event(struct virtio_video_device *vvd,
				      void *buf, unsigned int len)
{
	struct virtio_video_event_header *hdr = buf;
	uint32_t event_type;
	uint32_t stream_id;
	uint32_t cookie;
	uint32_t event_flags;
	bool is_standalone;
	unsigned long flags;
	struct virtio_video_async_response_entry *entry = NULL;
	struct virtio_video_stream *stream;

	if (len < sizeof(struct virtio_video_event_header)) {
		v4l2_warn(&vvd->v4l2_dev, "ignore short event buffer\n");
		return;
	}

	event_type = le32_to_cpu(hdr->event_type);
	stream_id = le32_to_cpu(hdr->stream_id);
	cookie = le32_to_cpu(hdr->async_response_cookie);
	event_flags = le32_to_cpu(hdr->event_flags);
	is_standalone = event_flags & VIRTIO_VIDEO_EVENT_FLAG_STANDALONE;

	if (!is_standalone && event_type != VIRTIO_VIDEO_EVENT_ERROR) {
		/* cookie == 0 means that the async response entry was not allocated */
		if (cookie) {
			spin_lock_irqsave(&vvd->async_resp_lock, flags);
			entry = idr_find(&vvd->async_resp_cookie_idr, cookie);
			if (!entry) {
				spin_unlock_irqrestore(&vvd->async_resp_lock,
						       flags);
				v4l2_err(
					&vvd->v4l2_dev,
					"stream_id=%u: async resp entry cookie %u not found\n",
					stream_id, cookie);
				return;
			}
			if (event_type != entry->type ||
			    stream_id != entry->stream->stream_id) {
				v4l2_err(
					&vvd->v4l2_dev,
					"stream_id=%u: async resp inconsistency detected cookie %u: type %u vs %u, stream_id %u vs %u\n",
					stream_id, cookie, event_type,
					entry->type, stream_id,
					entry->stream->stream_id);
				/* Release the device's reference. */
				// complete() ?
				virtio_video_release_async_response(vvd,
								    &entry);
				spin_unlock_irqrestore(&vvd->async_resp_lock,
						       flags);
				WARN_ON(true);
				return;
			}
			/* Make sure the entry is not freed before return from this
			 * function.
			 */
			refcount_inc(&entry->refcnt);
			spin_unlock_irqrestore(&vvd->async_resp_lock, flags);
		}
	} else if (cookie || !is_standalone ||
		   (event_type != VIRTIO_VIDEO_EVENT_ERROR &&
		    event_type != VIRTIO_VIDEO_ASYNC_RESP_STREAM_GET_PARAMS)) {
		v4l2_err(
			&vvd->v4l2_dev,
			"stream_id=%u: invalid event header cookie=%u event_type=%u event_flags=%u\n",
			stream_id, cookie, event_type, event_flags);
		return;
	}

	mutex_lock(&vvd->stream_idr_lock);
	stream = idr_find(&vvd->stream_idr, stream_id);
	if (!stream) {
		v4l2_warn(&vvd->v4l2_dev,
			  "stream_id=%u not found for event type %u\n",
			  stream_id, event_type);
		mutex_unlock(&vvd->stream_idr_lock);
		return;
	}

	switch (event_type) {
	case VIRTIO_VIDEO_EVENT_ERROR:
		v4l2_err(&vvd->v4l2_dev, "stream_id=%i: error event\n",
			 stream_id);
		virtio_video_state_update(stream, STREAM_STATE_ERROR);
		virtio_video_handle_error(stream);
		break;
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_RESOURCE_QUEUE: {
		struct virtio_video_resource_queue_async_resp *resp;
		if (len <
		    sizeof(struct virtio_video_resource_queue_async_resp)) {
			v4l2_warn(
				&vvd->v4l2_dev,
				"stream_id=%i: ignore short queue async resp\n",
				stream_id);
			break;
		}
		v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
			 "stream_id=%u: async response queue\n", stream_id);
		resp = (struct virtio_video_resource_queue_async_resp *)hdr;

		virtio_video_handle_async_response_queue(vvd, stream, resp,
							 entry, event_flags);
		break;
	}
	case VIRTIO_VIDEO_ASYNC_RESP_GET_CONTROL:
		BUG_ON(!entry || !entry->cb);
		entry->cb(vvd, entry, buf, len, event_flags);
		break;
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_GET_PARAMS:
		virtio_video_handle_async_response_get_params(vvd, stream, buf,
							      len, event_flags);
		if (is_standalone) {
			/* handle the standalone event - DRC */
			v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
				 "stream_id=%u: resolution change event\n",
				 stream_id);
			virtio_video_queue_res_chg_event(stream);
			if (virtio_video_state(stream) == STREAM_STATE_INIT) {
				virtio_video_state_update(
					stream,
					STREAM_STATE_DYNAMIC_RES_CHANGE);
				wake_up(&vvd->wq);
			}
		}
		break;
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_OPEN:
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_CLOSE:
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_SET_PARAMS:
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_DRAIN:
	case VIRTIO_VIDEO_ASYNC_RESP_STREAM_QUEUE_RESET:
	case VIRTIO_VIDEO_ASYNC_RESP_RESOURCE_ATTACH:
	case VIRTIO_VIDEO_ASYNC_RESP_QUEUE_DETACH_RESOURCES:
	case VIRTIO_VIDEO_ASYNC_RESP_SET_CONTROL:
		break;
	default:
		v4l2_warn(&vvd->v4l2_dev, "stream_id=%i: unknown event\n",
			  stream_id);
		break;
	}

	mutex_unlock(&vvd->stream_idr_lock);

	if (entry) {
		complete(&entry->reclaimed);
		/* Release this function's and the device's references if it is not
		 * released already.
		 */
		spin_lock_irqsave(&vvd->async_resp_lock, flags);
		if (!virtio_video_release_async_response(vvd, &entry))
			virtio_video_release_async_response(vvd, &entry);
		spin_unlock_irqrestore(&vvd->async_resp_lock, flags);
	}
}

int virtio_video_alloc_events(struct virtio_video_device *vvd)
{
	int ret;
	size_t i;
	uint8_t *buf;
	size_t num = vvd->eventq.vq->num_free;

	buf = kcalloc(num, EVENT_BUFFER_SIZE, GFP_KERNEL);
	if (!buf) {
		v4l2_err(&vvd->v4l2_dev, "failed to alloc event buffers!!!\n");
		return -ENOMEM;
	}
	vvd->events_buf = buf;

	for (i = 0; i < num; i++, buf += EVENT_BUFFER_SIZE) {
		ret = virtio_video_queue_event_buffer(vvd, buf,
						      EVENT_BUFFER_SIZE);
		if (ret) {
			v4l2_err(&vvd->v4l2_dev,
				 "failed to queue event buffer\n");
			return ret;
		}
	}

	virtqueue_kick(vvd->eventq.vq);

	return 0;
}

int virtio_video_cmd_stream_open(struct virtio_video_device *vvd,
				 uint32_t stream_id,
				 enum virtio_video_format format,
				 const char *tag)
{
	struct virtio_video_stream_open *req_p;
	struct virtio_video_vbuffer *vbuf;

	req_p = virtio_video_alloc_req(vvd, &vbuf, sizeof(*req_p));
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_OPEN);
	req_p->hdr.stream_id = cpu_to_le32(stream_id);
	req_p->in_mem_type = cpu_to_le32(VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES);
	req_p->out_mem_type = cpu_to_le32(VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES);
	req_p->coded_format = cpu_to_le32(format);
	if (strscpy(req_p->tag, tag, sizeof(req_p->tag) - 1) < 0)
		v4l2_err(&vvd->v4l2_dev, "failed to copy stream tag\n");
	req_p->tag[sizeof(req_p->tag) - 1] = 0;

	return virtio_video_queue_cmd_buffer(vvd, vbuf);
}

int virtio_video_cmd_stream_close(struct virtio_video_device *vvd,
				  struct virtio_video_stream *stream)
{
	struct virtio_video_stream_close *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_CLOSE);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_STREAM_CLOSE;

	return virtio_video_queue_cmd_buffer_wait_async_resp(vvd, vbuf, entry);
}

int virtio_video_cmd_stream_drain(struct virtio_video_device *vvd,
				  uint32_t stream_id)
{
	struct virtio_video_stream_drain *req_p;
	struct virtio_video_vbuffer *vbuf;

	req_p = virtio_video_alloc_req(vvd, &vbuf, sizeof(*req_p));
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_DRAIN);
	req_p->hdr.stream_id = cpu_to_le32(stream_id);

	return virtio_video_queue_cmd_buffer(vvd, vbuf);
}

int virtio_video_cmd_resource_attach(struct virtio_video_device *vvd,
				     uint32_t stream_id, uint32_t resource_id,
				     uint32_t queue_type, void *buf,
				     size_t buf_size)
{
	struct virtio_video_resource_attach *req_p;
	struct virtio_video_vbuffer *vbuf;

	req_p = virtio_video_alloc_req(vvd, &vbuf, sizeof(*req_p));
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_RESOURCE_ATTACH);
	req_p->hdr.stream_id = cpu_to_le32(stream_id);
	req_p->queue_type = cpu_to_le32(queue_type);
	req_p->resource_id = cpu_to_le32(resource_id);

	vbuf->data_buf = buf;
	vbuf->data_size = buf_size;

	return virtio_video_queue_cmd_buffer(vvd, vbuf);
}

int virtio_video_cmd_queue_detach_resources(struct virtio_video_device *vvd,
					    struct virtio_video_stream *stream,
					    uint32_t queue_type)
{
	struct virtio_video_queue_detach_resources *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);
	req_p->queue_type = cpu_to_le32(queue_type);

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_QUEUE_DETACH_RESOURCES;

	return virtio_video_queue_cmd_buffer_wait_async_resp(vvd, vbuf, entry);
}

static void virtio_video_handle_async_response_queue(
	struct virtio_video_device *vvd, struct virtio_video_stream *stream,
	struct virtio_video_resource_queue_async_resp *evt,
	struct virtio_video_async_response_entry *entry, uint32_t event_flags)
{
	uint32_t dequeue_flags;
	uint64_t timestamp;

	if (event_flags & VIRTIO_VIDEO_EVENT_FLAG_ERROR) {
		v4l2_err(&vvd->v4l2_dev, "buffer processing error\n");
		virtio_video_buf_done(entry->buf, VIRTIO_VIDEO_QUEUE_FLAG_ERR,
				      0, NULL);
	}

	dequeue_flags = le32_to_cpu(evt->flags);
	timestamp = le64_to_cpu(evt->timestamp);

	v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
		 "stream_id=%u: release buffer\n", stream->stream_id);
	virtio_video_buf_done(entry->buf, dequeue_flags, timestamp,
			      evt->data_sizes);
}

void virtio_video_cmd_resource_queue(struct virtio_video_device *vvd,
				     uint32_t stream_id,
				     struct virtio_video_buffer *virtio_vb)
{
	size_t i;
	struct virtio_video_resource_queue *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;
	struct vb2_buffer *vb = &virtio_vb->v4l2_m2m_vb.vb.vb2_buf;
	struct vb2_queue *vb2_queue = vb->vb2_queue;
	struct virtio_video_stream *stream = vb2_get_drv_priv(vb2_queue);

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p)) {
		v4l2_err(&vvd->v4l2_dev, "failed to alloc a buffer\n");
		virtio_video_buf_done(virtio_vb, VIRTIO_VIDEO_QUEUE_FLAG_ERR, 0,
				      NULL);
	}

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_RESOURCE_QUEUE);
	req_p->hdr.stream_id = cpu_to_le32(stream_id);
	req_p->hdr.queue_type = cpu_to_le32(to_virtio_queue_type(vb->type));
	req_p->resource_id = cpu_to_le32(virtio_vb->resource_id);
	req_p->flags = 0;
	req_p->timestamp = cpu_to_le64(vb->timestamp);

	for (i = 0; i < vb->num_planes; ++i) {
		req_p->offsets[i] = cpu_to_le32(vb->planes[i].data_offset);
		/* In V4L2 bytesused include the data_offset, but in virtio-video
		 * data_sizes don't include offsets.
		 */
		req_p->data_sizes[i] = cpu_to_le32(vb->planes[i].bytesused -
						   vb->planes[i].data_offset);
	}

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_STREAM_RESOURCE_QUEUE;
	entry->buf = virtio_vb;

	v4l2_dbg(1, virtio_video_debug_level(), &vvd->v4l2_dev,
		 "stream_id=%u: added async response, cookie %u\n",
		 stream->stream_id, entry->cookie);

	virtio_video_queue_cmd_buffer(vvd, vbuf);
}

int virtio_video_cmd_queue_reset(struct virtio_video_device *vvd,
				 struct virtio_video_stream *stream,
				 uint32_t queue_type)
{
	struct virtio_video_queue_reset *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_QUEUE_RESET);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);
	req_p->reset_queue_type = cpu_to_le32(queue_type);

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_STREAM_QUEUE_RESET;

	return virtio_video_queue_cmd_buffer_wait_async_resp(vvd, vbuf, entry);
}

int virtio_video_cmd_device_query_caps(struct virtio_video_device *vvd,
				       void *resp_buf, size_t resp_size,
				       uint32_t queue_type,
				       struct virtio_video_vbuffer **vbuf)
{
	struct virtio_video_device_query_caps *req_p;

	req_p = virtio_video_alloc_req_resp(vvd, vbuf, sizeof(*req_p),
					    resp_size, resp_buf);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->type = cpu_to_le32(VIRTIO_VIDEO_CMD_DEVICE_QUERY_CAPS);
	req_p->queue_type = cpu_to_le32(queue_type);

	return virtio_video_queue_cmd_buffer_sync(vvd, *vbuf, true);
}

int virtio_video_query_control_level(struct virtio_video_device *vvd,
				     void *resp_buf, size_t resp_size,
				     enum virtio_video_format format,
				     struct virtio_video_vbuffer **vbuf)
{
	struct virtio_video_query_control *req_p;
	struct virtio_video_query_control_level *ctrl_l;
	uint32_t req_size = 0;

	req_size = sizeof(struct virtio_video_query_control) +
		   sizeof(struct virtio_video_query_control_level);

	req_p = virtio_video_alloc_req_resp(vvd, vbuf, req_size, resp_size,
					    resp_buf);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->type = cpu_to_le32(VIRTIO_VIDEO_CMD_QUERY_CONTROL);
	req_p->control = cpu_to_le32(V4L2_CID_MPEG_VIDEO_H264_LEVEL);
	ctrl_l = (struct virtio_video_query_control_level *)(req_p + 1);
	ctrl_l->format = cpu_to_le32(format);

	return virtio_video_queue_cmd_buffer_sync(vvd, *vbuf, true);
}

int virtio_video_query_control_profile(struct virtio_video_device *vvd,
				       void *resp_buf, size_t resp_size,
				       enum virtio_video_format format,
				       struct virtio_video_vbuffer **vbuf)
{
	struct virtio_video_query_control *req_p;
	struct virtio_video_query_control_profile *ctrl_p;
	uint32_t req_size = 0;

	req_size = sizeof(struct virtio_video_query_control) +
		   sizeof(struct virtio_video_query_control_profile);

	req_p = virtio_video_alloc_req_resp(vvd, vbuf, req_size, resp_size,
					    resp_buf);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->type = cpu_to_le32(VIRTIO_VIDEO_CMD_QUERY_CONTROL);
	req_p->control = cpu_to_le32(V4L2_CID_MPEG_VIDEO_H264_PROFILE);
	ctrl_p = (struct virtio_video_query_control_profile *)(req_p + 1);
	ctrl_p->format = cpu_to_le32(format);

	return virtio_video_queue_cmd_buffer_sync(vvd, *vbuf, true);
}

static void virtio_video_handle_async_response_get_params(
	struct virtio_video_device *vvd, struct virtio_video_stream *stream,
	void *buf, unsigned int len, uint32_t event_flags)
{
	int i;
	struct virtio_video_stream_get_params_async_resp *resp =
		(struct virtio_video_stream_get_params_async_resp *)buf;
	struct virtio_video_params *params = &resp->params;
	uint32_t queue_type;
	struct video_format_info *format_info;

	if (len < sizeof(*resp)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp));
		return;
	}

	queue_type = le32_to_cpu(params->queue_type);
	if (queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT)
		format_info = &stream->in_info;
	else
		format_info = &stream->out_info;

	format_info->frame_rate = le32_to_cpu(params->frame_rate);
	format_info->frame_width = le32_to_cpu(params->frame_width);
	format_info->frame_height = le32_to_cpu(params->frame_height);
	format_info->min_buffers = le32_to_cpu(params->min_buffers);
	format_info->max_buffers = le32_to_cpu(params->max_buffers);
	format_info->fourcc_format =
		virtio_video_format_to_v4l2(le32_to_cpu(params->format));

	format_info->crop.top = le32_to_cpu(params->crop.top);
	format_info->crop.left = le32_to_cpu(params->crop.left);
	format_info->crop.width = le32_to_cpu(params->crop.width);
	format_info->crop.height = le32_to_cpu(params->crop.height);

	format_info->colorimetry.primaries =
		le32_to_cpu(params->colorimetry.primaries);
	format_info->colorimetry.transfer =
		le32_to_cpu(params->colorimetry.transfer);
	format_info->colorimetry.matrix =
		le32_to_cpu(params->colorimetry.matrix);
	format_info->colorimetry.range = le32_to_cpu(params->colorimetry.range);

	format_info->num_planes = le32_to_cpu(params->num_planes);
	for (i = 0; i < le32_to_cpu(params->num_planes); i++) {
		struct virtio_video_plane_format *plane_formats =
			&params->plane_formats[i];
		struct video_plane_format *plane_format =
			&format_info->plane_format[i];

		plane_format->plane_size =
			le32_to_cpu(plane_formats->plane_size);
		plane_format->stride = le32_to_cpu(plane_formats->stride);
	}
}

int virtio_video_cmd_stream_get_params(struct virtio_video_device *vvd,
				       struct virtio_video_stream *stream,
				       uint32_t queue_type)
{
	struct virtio_video_stream_get_params *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_GET_PARAMS);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);
	req_p->queue_type = cpu_to_le32(queue_type);

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_STREAM_GET_PARAMS;

	return virtio_video_queue_cmd_buffer_wait_async_resp(vvd, vbuf, entry);
}

int virtio_video_cmd_stream_set_params(struct virtio_video_device *vvd,
				       struct virtio_video_stream *stream,
				       struct video_format_info *format_info,
				       uint32_t queue_type)
{
	int i;
	struct virtio_video_stream_set_params *req_p;
	struct virtio_video_vbuffer *vbuf;

	req_p = virtio_video_alloc_req(vvd, &vbuf, sizeof(*req_p));
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_STREAM_SET_PARAMS);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);
	req_p->params.queue_type = cpu_to_le32(queue_type);
	req_p->params.frame_rate = cpu_to_le32(format_info->frame_rate);
	req_p->params.frame_width = cpu_to_le32(format_info->frame_width);
	req_p->params.frame_height = cpu_to_le32(format_info->frame_height);
	req_p->params.format = virtio_video_v4l2_format_to_virtio(
		cpu_to_le32(format_info->fourcc_format));
	req_p->params.min_buffers = cpu_to_le32(format_info->min_buffers);
	req_p->params.max_buffers = cpu_to_le32(format_info->max_buffers);
	req_p->params.num_planes = cpu_to_le32(format_info->num_planes);

	req_p->params.colorimetry.primaries =
		cpu_to_le32(format_info->colorimetry.primaries);
	req_p->params.colorimetry.transfer =
		cpu_to_le32(format_info->colorimetry.transfer);
	req_p->params.colorimetry.matrix =
		cpu_to_le32(format_info->colorimetry.matrix);
	req_p->params.colorimetry.range =
		cpu_to_le32(format_info->colorimetry.range);

	for (i = 0; i < format_info->num_planes; i++) {
		struct virtio_video_plane_format *plane_formats =
			&req_p->params.plane_formats[i];
		struct video_plane_format *plane_format =
			&format_info->plane_format[i];
		plane_formats->plane_size =
			cpu_to_le32(plane_format->plane_size);
		plane_formats->stride = cpu_to_le32(plane_format->stride);
	}

	return virtio_video_queue_cmd_buffer(vvd, vbuf);
}

static void virtio_video_cmd_get_ctrl_profile_cb(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags)
{
	struct virtio_video_get_control_async_resp *resp =
		(struct virtio_video_get_control_async_resp *)buf;
	struct virtio_video_control_val_profile *resp_p =
		(struct virtio_video_control_val_profile *)(resp + 1);
	struct video_control_info *control = &entry->stream->control;

	if (len < sizeof(*resp) + sizeof(*resp_p)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp) + sizeof(*resp_p));
		return;
	}

	control->profile = le32_to_cpu(resp_p->profile);
}

static void virtio_video_cmd_get_ctrl_level_cb(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags)
{
	struct virtio_video_get_control_async_resp *resp =
		(struct virtio_video_get_control_async_resp *)buf;
	struct virtio_video_control_val_level *resp_p =
		(struct virtio_video_control_val_level *)(resp + 1);
	struct video_control_info *control = &entry->stream->control;

	if (len < sizeof(*resp) + sizeof(*resp_p)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp) + sizeof(*resp_p));
		return;
	}

	control->level = le32_to_cpu(resp_p->level);
}

static void virtio_video_cmd_get_ctrl_bitrate_cb(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags)
{
	struct virtio_video_get_control_async_resp *resp =
		(struct virtio_video_get_control_async_resp *)buf;
	struct virtio_video_control_val_bitrate *resp_p =
		(struct virtio_video_control_val_bitrate *)(resp + 1);
	struct video_control_info *control = &entry->stream->control;

	if (len < sizeof(*resp) + sizeof(*resp_p)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp) + sizeof(*resp_p));
		return;
	}

	control->bitrate = le32_to_cpu(resp_p->bitrate);
}

static void virtio_video_cmd_get_ctrl_dec_display_delay_enable_cb(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags)
{
	struct virtio_video_get_control_async_resp *resp =
		(struct virtio_video_get_control_async_resp *)buf;
	struct virtio_video_control_val_dec_display_delay_enable *resp_p =
		(struct virtio_video_control_val_dec_display_delay_enable
			 *)(resp + 1);
	struct video_control_info *control = &entry->stream->control;

	if (len < sizeof(*resp) + sizeof(*resp_p)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp) + sizeof(*resp_p));
		return;
	}

	control->display_delay_enable = le32_to_cpu(resp_p->delay_enable);
}

static void virtio_video_cmd_get_ctrl_dec_display_delay_cb(
	struct virtio_video_device *vvd,
	struct virtio_video_async_response_entry *entry, void *buf,
	unsigned int len, uint32_t event_flags)
{
	struct virtio_video_get_control_async_resp *resp =
		(struct virtio_video_get_control_async_resp *)buf;
	struct virtio_video_control_val_dec_display_delay *resp_p =
		(struct virtio_video_control_val_dec_display_delay *)(resp + 1);
	struct video_control_info *control = &entry->stream->control;

	if (len < sizeof(*resp) + sizeof(*resp_p)) {
		v4l2_err(&vvd->v4l2_dev, "async response len %u needed %lu\n",
			 len, sizeof(*resp) + sizeof(*resp_p));
		return;
	}

	control->display_delay = le32_to_cpu(resp_p->delay);
}

int virtio_video_cmd_get_control(struct virtio_video_device *vvd,
				 struct virtio_video_stream *stream,
				 uint32_t control)
{
	struct virtio_video_get_control *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_async_response_entry *entry;
	virtio_video_async_resp_cb cb;

	switch (control) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		cb = &virtio_video_cmd_get_ctrl_profile_cb;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		cb = &virtio_video_cmd_get_ctrl_level_cb;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		cb = &virtio_video_cmd_get_ctrl_bitrate_cb;
		break;
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE:
		cb = &virtio_video_cmd_get_ctrl_dec_display_delay_enable_cb;
		break;
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY:
		cb = &virtio_video_cmd_get_ctrl_dec_display_delay_cb;
		break;
	default:
		return -EINVAL;
	}

	req_p = virtio_video_alloc_stream_cmd_async_resp(
		vvd, &vbuf, sizeof(*req_p), &entry, stream);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_GET_CONTROL);
	req_p->hdr.stream_id = cpu_to_le32(stream->stream_id);
	req_p->control = cpu_to_le32(control);

	entry->type = VIRTIO_VIDEO_ASYNC_RESP_GET_CONTROL;
	entry->cb = cb;

	return virtio_video_queue_cmd_buffer_wait_async_resp(vvd, vbuf, entry);
}

int virtio_video_cmd_set_control(struct virtio_video_device *vvd,
				 uint32_t stream_id, uint32_t control,
				 uint32_t value)
{
	struct virtio_video_set_control *req_p;
	struct virtio_video_vbuffer *vbuf;
	struct virtio_video_tlv *outer_tlv, *v4l2_tlv, *control_tlv;
	size_t size;
	uint32_t outer_type;

	switch (control) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		size = sizeof(struct virtio_video_tlv_v4l2_enum_val);
		outer_type = VIRTIO_VIDEO_TLV_CODED_SET;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		size = sizeof(struct virtio_video_tlv_v4l2_int_val);
		outer_type = VIRTIO_VIDEO_TLV_CODED_SET;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		/* This TLV is empty */
		size = 0;
		outer_type = VIRTIO_VIDEO_TLV_CODED_SET;
		break;
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE:
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY:
		size = sizeof(struct virtio_video_tlv_v4l2_int_val);
		outer_type = VIRTIO_VIDEO_TLV_RAW_SET;
		break;
	default:
		return -EINVAL;
	}

	req_p = virtio_video_alloc_req(
		vvd, &vbuf,
		sizeof(*req_p) + 3 * sizeof(struct virtio_video_tlv) + size);
	if (IS_ERR(req_p))
		return PTR_ERR(req_p);

	req_p->hdr.type = cpu_to_le32(VIRTIO_VIDEO_CMD_SET_CONTROL);
	req_p->hdr.stream_id = cpu_to_le32(stream_id);
	outer_tlv = (struct virtio_video_tlv *)(req_p + 1);
	outer_tlv->type = cpu_to_le32(outer_type);
	outer_tlv->length =
		cpu_to_le32(2 * sizeof(struct virtio_video_tlv) + size);
	v4l2_tlv = outer_tlv + 1;
	v4l2_tlv->type = cpu_to_le32(VIRTIO_VIDEO_TLV_V4L2_CONTROLS);
	v4l2_tlv->length = cpu_to_le32(sizeof(struct virtio_video_tlv) + size);
	control_tlv = v4l2_tlv + 1;
	control_tlv->type = cpu_to_le32(control);
	control_tlv->length = cpu_to_le32(size);

	switch (control) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL: {
		struct virtio_video_tlv_v4l2_enum_val *enum_val =
			(void *)(control_tlv + 1);

		enum_val->value = cpu_to_le32(value);
		break;
	}
	case V4L2_CID_MPEG_VIDEO_BITRATE:
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE:
	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY: {
		struct virtio_video_tlv_v4l2_int_val *int_val =
			(void *)(control_tlv + 1);

		int_val->value = cpu_to_le32(value);
		break;
	}
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		// Button controls have no value.
		break;
	}

	return virtio_video_queue_cmd_buffer(vvd, vbuf);
}
