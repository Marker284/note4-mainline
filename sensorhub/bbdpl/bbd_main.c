/*
 * Copyright 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * The BBD (Broadcom Bridge Driver)
 *
 * Linux 7.3 port: built as module bbd.ko together with the SSI-SPI
 * transport (bcm_gps_spi.c).  Char devices keep major 239 and the
 * downstream minors/names so the closed LHD daemon sees the same ABI:
 *   /dev/bbd_sensor (0) /dev/bbd_control (1) /dev/bbd_packet (2)
 *   /dev/bbd_reliable (3) /dev/bbd_patch (4) /dev/bbd_ssi_spi_debug (5)
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "bbd_internal.h"

struct bbd_device *gpbbd_dev;

static int bbd_major = BBD_DEVICE_MAJOR;
module_param_named(major, bbd_major, int, 0444);
MODULE_PARM_DESC(major, "char major for /dev/bbd_* (downstream: 239, 0 = dynamic)");

static const struct file_operations *const bbd_fops[BBD_DEVICE_INDEX] = {
	[BBD_MINOR_SENSOR]	  = &bbd_sensor_fops,
	[BBD_MINOR_CONTROL]	  = &bbd_control_fops,
	[BBD_MINOR_PACKET]	  = &bbd_packet_fops,
	[BBD_MINOR_RELIABLE]	  = &bbd_reliable_fops,
	[BBD_MINOR_PATCH]	  = &bbd_patch_fops,
	[BBD_MINOR_SSI_SPI_DEBUG] = &bbd_ssi_spi_debug_fops,
};

static const char *const bbd_dev_name[BBD_DEVICE_INDEX] = {
	"bbd_sensor",
	"bbd_control",
	"bbd_packet",
	"bbd_reliable",
	"bbd_patch",
	"bbd_ssi_spi_debug",
};

/* /sys/devices/platform/bbd (sysfs controls used by LHD / for debugging) */
static struct platform_device *bbd_pdev;
static const struct class bbd_class = {
	.name = "bbd",
};
static dev_t bbd_devt;

static int bbd_init_vars(struct bbd_device *pbbd_dev)
{
	int result = 0;

	pbbd_dev->sio_type = BBD_SERIAL_SPI;
	pbbd_dev->shmd     = true;
	pbbd_dev->tty_baud = 921600;
	mutex_init(&pbbd_dev->qlock);
	mutex_init(&pbbd_dev->cb_lock);

	bbd_sensor_init_vars  (pbbd_dev, &result);
	bbd_control_init_vars (pbbd_dev, &result);
	bbd_packet_init_vars  (pbbd_dev, &result);
	bbd_reliable_init_vars(pbbd_dev, &result);
	bbd_patch_init_vars   (pbbd_dev, &result);
	bbd_ssi_spi_debug_init_vars(pbbd_dev, &result);
	if (!result)
		BbdEngine_BbdEngine(&pbbd_dev->bbd_engine);
	return result;
}

static void bbd_free_vars(void)
{
	bbd_patch_uninit();
	bbd_reliable_uninit();
	bbd_packet_uninit();
	bbd_control_uninit();
	bbd_sensor_uninit();
	if (gpbbd_dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG]) {
		bbd_base_uninit(gpbbd_dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG]);
		bbd_free(gpbbd_dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG]);
		gpbbd_dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG] = NULL;
	}
	bbd_qitem_cache_uninit();
}

static int bbd_device_init(void)
{
	int i, ret;

	gpbbd_dev = bbd_alloc(sizeof(struct bbd_device));
	if (!gpbbd_dev)
		return -ENOMEM;

	ret = bbd_init_vars(gpbbd_dev);
	if (ret < 0)
		goto free_vars;

	if (bbd_major) {
		bbd_devt = MKDEV(bbd_major, 0);
		ret = register_chrdev_region(bbd_devt, BBD_DEVICE_INDEX, "bbd");
		if (ret) {
			pr_warn("BBD: major %d busy (%d), using a dynamic one\n",
				bbd_major, ret);
			bbd_major = 0;
		}
	}
	if (!bbd_major) {
		ret = alloc_chrdev_region(&bbd_devt, 0, BBD_DEVICE_INDEX, "bbd");
		if (ret)
			goto free_vars;
	}

	ret = class_register(&bbd_class);
	if (ret)
		goto unregister_region;

	for (i = 0; i < BBD_DEVICE_INDEX; i++) {
		struct device *dev;

		cdev_init(&gpbbd_dev->dev[i], bbd_fops[i]);
		gpbbd_dev->dev[i].owner = THIS_MODULE;
		ret = cdev_add(&gpbbd_dev->dev[i], bbd_devt + i, 1);
		if (ret)
			goto destroy_devs;
		dev = device_create(&bbd_class, NULL, bbd_devt + i, NULL,
				    "%s", bbd_dev_name[i]);
		if (IS_ERR(dev)) {
			cdev_del(&gpbbd_dev->dev[i]);
			ret = PTR_ERR(dev);
			goto destroy_devs;
		}
		dprint("/dev/%s (%d,%d)\n", bbd_dev_name[i],
		       MAJOR(bbd_devt), MINOR(bbd_devt) + i);
	}

	bbd_pdev = platform_device_register_simple("bbd", -1, NULL, 0);
	if (IS_ERR(bbd_pdev)) {
		ret = PTR_ERR(bbd_pdev);
		bbd_pdev = NULL;
		goto destroy_devs;
	}
	dev_set_drvdata(&bbd_pdev->dev, gpbbd_dev);
	gpbbd_dev->pdev = &bbd_pdev->dev;

	ret = bbd_sysfs_init(&bbd_pdev->dev.kobj);
	if (ret)
		goto unreg_platform_dev;

	pr_info("BBD: char devices ready (major %d)\n", MAJOR(bbd_devt));
	return 0;

unreg_platform_dev:
	platform_device_unregister(bbd_pdev);
	bbd_pdev = NULL;
	i = BBD_DEVICE_INDEX;
destroy_devs:
	while (--i >= 0) {
		device_destroy(&bbd_class, bbd_devt + i);
		cdev_del(&gpbbd_dev->dev[i]);
	}
	class_unregister(&bbd_class);
unregister_region:
	unregister_chrdev_region(bbd_devt, BBD_DEVICE_INDEX);
free_vars:
	bbd_free_vars();
	bbd_free(gpbbd_dev);
	gpbbd_dev = NULL;
	return ret;
}

static void bbd_device_exit(void)
{
	int i;

	if (!gpbbd_dev)
		return;

	bbd_sysfs_uninit(&bbd_pdev->dev.kobj);
	bbd_patch_uninit();
	gpbbd_dev->pdev = NULL;
	platform_device_unregister(bbd_pdev);

	for (i = 0; i < BBD_DEVICE_INDEX; i++) {
		device_destroy(&bbd_class, bbd_devt + i);
		cdev_del(&gpbbd_dev->dev[i]);
	}
	class_unregister(&bbd_class);
	unregister_chrdev_region(bbd_devt, BBD_DEVICE_INDEX);

	BbdEngine_dtor(&gpbbd_dev->bbd_engine);
	bbd_ssi_spi_close();
	bbd_free_vars();

	pr_info("BBD: %d buffers lost\n", bbd_alloc_count() - 1);
	bbd_free(gpbbd_dev);
	gpbbd_dev = NULL;
}

static int __init bbd_module_init(void)
{
	int ret;

	/* bbd core must exist before the SPI transport probes */
	ret = bbd_device_init();
	if (ret)
		return ret;

	ret = bcm_gps_spi_init();
	if (ret)
		bbd_device_exit();
	return ret;
}

static void __exit bbd_module_exit(void)
{
	bcm_gps_spi_exit();
	bbd_device_exit();
}

module_init(bbd_module_init);
module_exit(bbd_module_exit);

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("Broadcom BCM4773 bridge driver (BBD) + SSI-SPI transport");
MODULE_LICENSE("Dual BSD/GPL");
