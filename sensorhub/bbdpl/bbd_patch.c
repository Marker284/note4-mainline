/*
 * Copyright 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * The BBD (Broadcom Bridge Driver)
 *
 * /dev/bbd_patch: serves the BCM4773 sensor-hub patch to LHD.
 *
 * Downstream compiled the 290 KiB patch (bbd_patch_file.h, a text container
 * "<FormatVersion=...>\n...<Blob=HEX...>") into the kernel as a char array
 * and LHD read it back through this node.  The port loads the very same
 * bytes with request_firmware() instead (default name below, module
 * parameter "patch_fw").  tools/extract_bbd_patch.py turns the downstream
 * header into that file, byte-identical including the trailing NUL that
 * sizeof() of the downstream array contained.  Building with
 * BBD_EMBED_PATCH=<path to bbd_patch_file.h> restores the old behaviour.
 */

#include <linux/firmware.h>
#include "bbd_internal.h"

char *bbd_patch_fw_name = "bcm4773/bcm_sensorhub_a2.patch";
module_param_named(patch_fw, bbd_patch_fw_name, charp, 0644);
MODULE_PARM_DESC(patch_fw, "firmware file served by /dev/bbd_patch");

#ifdef BBD_EMBED_PATCH
static const unsigned char bbd_new_patch[] =
{
#include BBD_EMBED_PATCH
};
#endif

static DEFINE_MUTEX(bbd_patch_lock);
static const struct firmware *bbd_patch_fw;

void bbd_patch_init_vars(struct bbd_device *dev, int* result)
{
	dev->bbd_ptr[BBD_MINOR_PATCH] = NULL;
}

int bbd_patch_uninit(void)
{
	mutex_lock(&bbd_patch_lock);
	release_firmware(bbd_patch_fw);
	bbd_patch_fw = NULL;
	mutex_unlock(&bbd_patch_lock);
	return 0;
}

static int bbd_patch_get(const u8 **data, size_t *size)
{
	int ret = 0;

	mutex_lock(&bbd_patch_lock);
	if (!bbd_patch_fw) {
		ret = request_firmware(&bbd_patch_fw, bbd_patch_fw_name,
				       gpbbd_dev->pdev);
		if (ret) {
			bbd_patch_fw = NULL;
#ifdef BBD_EMBED_PATCH
			*data = bbd_new_patch;
			*size = sizeof(bbd_new_patch);
			ret = 0;
#else
			pr_err("BBD: cannot load patch %s (%d)\n",
			       bbd_patch_fw_name, ret);
#endif
			goto out;
		}
	}
	*data = bbd_patch_fw->data;
	*size = bbd_patch_fw->size;
out:
	mutex_unlock(&bbd_patch_lock);
	return ret;
}

static int bbd_patch_open(struct inode *inode, struct file *filp)
{
	const u8 *data;
	size_t size;
	int ret;

	if (!gpbbd_dev)
		return -EFAULT;

	ret = bbd_patch_get(&data, &size);
	if (ret)
		return ret;

	pr_info("BBD:%s() %zu\n", __func__, size);
	filp->private_data = gpbbd_dev;
	return 0;
}

static int bbd_patch_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t bbd_patch_read(struct file *filp, char __user *buf,
			      size_t size, loff_t *ppos)
{
	const u8 *data;
	size_t len;

	if (bbd_patch_get(&data, &len))
		return -EIO;

	/* downstream reset *ppos to 0 at EOF; keep that quirk */
	if (*ppos >= len) {
		*ppos = 0;
		return 0;
	}
	return simple_read_from_buffer(buf, size, ppos, data, len);
}

const struct file_operations bbd_patch_fops = {
	.owner		= THIS_MODULE,
	.open		= bbd_patch_open,
	.release	= bbd_patch_release,
	.read		= bbd_patch_read,
	.llseek		= default_llseek,
};
