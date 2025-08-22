/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*
 * Driver for virtio video device.
 */

#include <linux/module.h>
#include <linux/version.h>

#include "virtio_video.h"

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "debug level (0-2)");

static unsigned int use_dma_mem;
module_param(use_dma_mem, uint, 0644);
MODULE_PARM_DESC(use_dma_mem, "Try to allocate buffers from the DMA zone");

static int vid_nr_dec = -1;
module_param(vid_nr_dec, int, 0644);
MODULE_PARM_DESC(vid_nr_dec, "videoN start number, -1 is autodetect");

static int vid_nr_enc = -1;
module_param(vid_nr_enc, int, 0644);
MODULE_PARM_DESC(vid_nr_enc, "videoN start number, -1 is autodetect");

unsigned int virtio_video_debug_level(void)
{
	return debug;
}

static int virtio_video_probe(struct virtio_device *vdev)
{
	int ret;
	struct virtio_video_device *vvd;
	struct virtqueue *vqs[2];
	struct device *dev = &vdev->dev;
	struct device *pdev = dev->parent;

	static const char * const names[] = { "commandq", "eventq" };
	static vq_callback_t *callbacks[] = {
		virtio_video_cmd_cb,
		virtio_video_event_cb
	};

	if (!virtio_has_feature(vdev, VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES)) {
		dev_err(dev, "device must support guest allocated buffers\n");
		return -ENODEV;
	}

	vvd = devm_kzalloc(dev, sizeof(*vvd), GFP_KERNEL);
	if (!vvd)
		return -ENOMEM;

	switch (vdev->id.device) {
	case VIRTIO_ID_VIDEO_ENC:
		vvd->vid_dev_nr = vid_nr_enc;
		vvd->type = VIRTIO_VIDEO_DEVICE_ENCODER;
		break;
	case VIRTIO_ID_VIDEO_DEC:
	default:
		vvd->vid_dev_nr = vid_nr_dec;
		vvd->type = VIRTIO_VIDEO_DEVICE_DECODER;
		break;
	}

	vvd->vdev = vdev;
	vvd->use_dma_mem = use_dma_mem;
	vdev->priv = vvd;

	spin_lock_init(&vvd->resource_idr_lock);
	idr_init(&vvd->resource_idr);
	spin_lock_init(&vvd->stream_idr_lock);
	idr_init(&vvd->stream_idr);

	init_waitqueue_head(&vvd->wq);

	if (virtio_has_feature(vdev, VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG))
		vvd->supp_non_contig = true;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
	vvd->has_iommu = !virtio_has_dma_quirk(vdev);
#else
	vvd->has_iommu = !virtio_has_iommu_quirk(vdev);
#endif

	dev_set_name(dev, "%s.%i", DRIVER_NAME, vdev->index);
	ret = v4l2_device_register(pdev, &vvd->v4l2_dev);
	if (ret)
		goto err_v4l2_reg;

	spin_lock_init(&vvd->commandq.qlock);
	init_waitqueue_head(&vvd->commandq.reclaim_queue);

	INIT_WORK(&vvd->eventq.work, virtio_video_process_events);

	INIT_LIST_HEAD(&vvd->pending_vbuf_list);

	ret = virtio_find_vqs(vdev, 2, vqs, callbacks, names, NULL);
	if (ret) {
		v4l2_err(&vvd->v4l2_dev, "failed to find virt queues\n");
		goto err_vqs;
	}

	vvd->commandq.vq = vqs[0];
	vvd->eventq.vq = vqs[1];

	ret = virtio_video_alloc_vbufs(vvd);
	if (ret) {
		v4l2_err(&vvd->v4l2_dev, "failed to alloc vbufs\n");
		goto err_vbufs;
	}

	virtio_cread(vdev, struct virtio_video_config, max_caps_length,
		     &vvd->max_caps_len);
	if (!vvd->max_caps_len) {
		v4l2_err(&vvd->v4l2_dev, "max_caps_len is zero\n");
		ret = -EINVAL;
		goto err_config;
	}

	virtio_cread(vdev, struct virtio_video_config, max_resp_length,
		     &vvd->max_resp_len);
	if (!vvd->max_resp_len) {
		v4l2_err(&vvd->v4l2_dev, "max_resp_len is zero\n");
		ret = -EINVAL;
		goto err_config;
	}

	ret = virtio_video_alloc_events(vvd);
	if (ret)
		goto err_events;

	virtio_device_ready(vdev);
	vvd->commandq.ready = true;
	vvd->eventq.ready = true;

	ret = virtio_video_device_init(vvd);
	if (ret) {
		v4l2_err(&vvd->v4l2_dev,
			 "failed to init virtio video\n");
		goto err_init;
	}

	return 0;

err_init:
err_events:
err_config:
	virtio_video_free_vbufs(vvd);
err_vbufs:
	vdev->config->del_vqs(vdev);
err_vqs:
	v4l2_device_unregister(&vvd->v4l2_dev);
err_v4l2_reg:
	devm_kfree(&vdev->dev, vvd);

	return ret;
}

static void virtio_video_remove(struct virtio_device *vdev)
{
	struct virtio_video_device *vvd = vdev->priv;

	virtio_video_device_deinit(vvd);
	virtio_video_free_vbufs(vvd);
	vdev->config->del_vqs(vdev);
	v4l2_device_unregister(&vvd->v4l2_dev);
	devm_kfree(&vdev->dev, vvd);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_VIDEO_DEC, VIRTIO_DEV_ANY_ID },
	{ VIRTIO_ID_VIDEO_ENC, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES,
	VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG,
};

static struct virtio_driver virtio_video_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = DRIVER_NAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.probe = virtio_video_probe,
	.remove = virtio_video_remove,
};

module_virtio_driver(virtio_video_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("virtio video driver");
MODULE_AUTHOR("Qualcomm Technologies, Inc. and/or its subsidiaries");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
