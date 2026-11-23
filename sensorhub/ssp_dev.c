/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Linux 7.3 port: ssp_bbd.ko binds to the "bbd-ssp" platform device that
 *  bbd.ko creates below the BCM4773 SPI device (downstream called
 *  pssp_driver->probe(spi) from bcm4773_spi_probe()).  The DT node is the
 *  SPI node, shared with bbd.ko.
 */
#include "ssp.h"
#include <linux/of.h>
#include <linux/property.h>
#include <linux/pm_wakeup.h>

#define NORMAL_SENSOR_STATE_K	0x3FEFF

void ssp_enable(struct ssp_data *data, bool enable)
{
	if (enable == !data->bSspShutdown)
		return;

	pr_info("[SSP] %s, new enable = %d, old enable = %d\n",
		__func__, enable, !data->bSspShutdown);

	data->bSspShutdown = !enable;
}

/*************************************************************************/
/* initialize sensor hub						 */
/*************************************************************************/

static int initialize_variable(struct ssp_data *data)
{
	int iSensorIndex;

	for (iSensorIndex = 0; iSensorIndex < SENSOR_MAX; iSensorIndex++) {
		data->adDelayBuf[iSensorIndex] = DEFUALT_POLLING_DELAY;
		data->batchLatencyBuf[iSensorIndex] = 0;
		data->batchOptBuf[iSensorIndex] = 0;
		data->aiCheckStatus[iSensorIndex] = INITIALIZATION_STATE;
		data->lastTimestamp[iSensorIndex] = 0;
		data->reportedData[iSensorIndex] = false;
	}

	atomic_set(&data->aSensorEnable, 0);
	data->uSensorState = NORMAL_SENSOR_STATE_K;

	data->bSspShutdown = true;
	data->bMcuReady = false;
	data->bProximityRawEnabled = false;
	data->bTimeSyncing = true;
	data->bHandlingIrq = false;

	data->uProxHiThresh = data->uProxHiThresh_default;
	data->uProxLoThresh = data->uProxLoThresh_default;
	data->uGyroDps = GYROSCOPE_DPS2000;

	data->bMcuDumpMode = ssp_check_sec_dump_mode();
	INIT_LIST_HEAD(&data->pending_list);
	init_waitqueue_head(&data->data_wq);

	data->bbd_on_packet_wq =
		alloc_ordered_workqueue("ssp_bbd_on_packet_wq", WQ_MEM_RECLAIM);
	INIT_WORK(&data->work_bbd_on_packet, bbd_on_packet_work_func);

	data->bbd_mcu_ready_wq =
		alloc_ordered_workqueue("ssp_bbd_mcu_ready_wq", WQ_MEM_RECLAIM);
	INIT_WORK(&data->work_bbd_mcu_ready, bbd_mcu_ready_work_func);

	if (!data->bbd_on_packet_wq || !data->bbd_mcu_ready_wq)
		return -ENOMEM;

	data->step_count_total = 0;
	initialize_function_pointer(data);
	return 0;
}

int initialize_mcu(struct ssp_data *data)
{
	int iRet = 0;

	clean_pending_list(data);

	iRet = get_chipid(data);
	pr_info("[SSP] MCU device ID = %d, reading ID = %d\n", DEVICE_ID, iRet);
	if (iRet != DEVICE_ID) {
		if (iRet < 0) {
			pr_err("[SSP]: %s - MCU is not working : 0x%x\n",
				__func__, iRet);
		} else {
			pr_err("[SSP]: %s - MCU identification failed\n",
				__func__);
			iRet = -ENODEV;
		}
		goto out;
	}

	iRet = set_sensor_position(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - set_sensor_position failed\n", __func__);
		goto out;
	}

	data->uSensorState = get_sensor_scanning_info(data);
	if (data->uSensorState == 0) {
		pr_err("[SSP]: %s - get_sensor_scanning_info failed\n",
			__func__);
		iRet = ERROR;
		goto out;
	}

	iRet = initialize_magnetic_sensor(data);
	if (iRet < 0)
		pr_err("[SSP]: %s - initialize magnetic sensor failed\n",
			__func__);

	data->uCurFirmRev = get_firmware_rev(data);
	pr_info("[SSP] MCU Firm Rev : New = %8u\n",
		data->uCurFirmRev);

	/* downstream skipped MSG2SSP_AP_MCU_DUMP_CHECK for BBD as well */
	iRet = SUCCESS;
out:
	return iRet;
}

static bbd_callbacks ssp_bbd_callbacks = {
	.on_packet		= NULL,
	.on_packet_alarm	= callback_bbd_on_packet_alarm,
	.on_control		= callback_bbd_on_control,
	.on_mcu_ready		= callback_bbd_on_mcu_ready,
};

static int ssp_parse_dt(struct device *dev, struct ssp_data *data)
{
	u32 matrix[9];
	int i;

	if (device_property_read_u32(dev, "ssp-acc-position", &data->accel_position))
		data->accel_position = 0;
	if (device_property_read_u32(dev, "ssp-mag-position", &data->mag_position))
		data->mag_position = 0;

	pr_info("[SSP] acc-posi[%d] mag-posi[%d]\n",
		data->accel_position, data->mag_position);

	if (device_property_read_u32(dev, "ssp-ap-rev", &data->ap_rev))
		data->ap_rev = 0;

	if (device_property_read_u32(dev, "ssp,prox-hi_thresh",
				     &data->uProxHiThresh_default))
		data->uProxHiThresh_default = DEFUALT_HIGH_THRESHOLD;

	if (device_property_read_u32(dev, "ssp,prox-low_thresh",
				     &data->uProxLoThresh_default))
		data->uProxLoThresh_default = DEFUALT_LOW_THRESHOLD;

	pr_info("[SSP] hi-thresh[%u] low-thresh[%u]\n",
		data->uProxHiThresh_default, data->uProxLoThresh_default);

	/* YAS532 static matrix: 9 cells, negative values as (-x) */
	if (!device_property_read_u32_array(dev, "ssp-mag-array", matrix, 9)) {
		data->static_matrix = devm_kcalloc(dev, 9, sizeof(s16), GFP_KERNEL);
		if (!data->static_matrix)
			return -ENOMEM;
		for (i = 0; i < 9; i++)
			data->static_matrix[i] = (s16)(s32)matrix[i];
	} else {
		pr_info("[SSP] No static matrix at DT for YAS532\n");
	}

	return 0;
}

static void ssp_destroy_wqs(struct ssp_data *data)
{
	if (data->bbd_on_packet_wq)
		destroy_workqueue(data->bbd_on_packet_wq);
	if (data->bbd_mcu_ready_wq)
		destroy_workqueue(data->bbd_mcu_ready_wq);
	if (data->debug_wq)
		destroy_workqueue(data->debug_wq);
}

static int ssp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ssp_data *data;
	int iRet;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;

	iRet = ssp_parse_dt(dev, data);
	if (iRet)
		return iRet;

	data->bProbeIsDone = false;
	platform_set_drvdata(pdev, data);

	mutex_init(&data->comm_mutex);
	mutex_init(&data->pending_mutex);
	mutex_init(&data->enable_mutex);

	iRet = initialize_variable(data);
	if (iRet)
		goto err_wq;

	device_init_wakeup(dev, true);

	iRet = initialize_debug_timer(data);
	if (iRet < 0) {
		iRet = -ENOMEM;
		goto err_wq;
	}

	iRet = initialize_sysfs(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create sysfs\n", __func__);
		goto err_wq;
	}

	iRet = ssp_iio_init(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create iio devices\n", __func__);
		goto err_sysfs;
	}

	bbd_register(data, &ssp_bbd_callbacks);

	/* LHD may have announced ESW:READY before we were loaded */
	if (bbd_esw_ready())
		callback_bbd_on_mcu_ready(data, true);

	pr_info("[SSP]: %s - probe success! (waiting for ESW:READY from LHD)\n",
		__func__);

	enable_debug_timer(data);
	data->bProbeIsDone = true;
	return 0;

err_sysfs:
	remove_sysfs(data);
err_wq:
	device_init_wakeup(dev, false);
	ssp_destroy_wqs(data);
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
	mutex_destroy(&data->enable_mutex);
	pr_err("[SSP] %s, probe failed! %d\n", __func__, iRet);
	return iRet;
}

static void ssp_remove(struct platform_device *pdev)
{
	struct ssp_data *data = platform_get_drvdata(pdev);

	disable_debug_timer(data);
	bbd_register(NULL, NULL);

	cancel_work_sync(&data->work_bbd_mcu_ready);
	cancel_work_sync(&data->work_bbd_on_packet);

	data->bMcuReady = false;
	ssp_enable(data, false);
	clean_pending_list(data);

	ssp_iio_remove(data);
	remove_sysfs(data);

	device_init_wakeup(&pdev->dev, false);
	ssp_destroy_wqs(data);
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
	mutex_destroy(&data->enable_mutex);
}

static void ssp_shutdown(struct platform_device *pdev)
{
	struct ssp_data *data = platform_get_drvdata(pdev);

	if (!data || !data->bProbeIsDone)
		return;

	disable_debug_timer(data);
	if (data->bMcuReady &&
	    SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_SHUTDOWN, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_SHUTDOWN failed\n",
			__func__);
	ssp_enable(data, false);
	clean_pending_list(data);
}

static int ssp_suspend(struct device *dev)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	func_dbg();

	if (data->bMcuReady &&
	    SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_SUSPEND, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_SUSPEND failed\n",
			__func__);

	data->uLastResumeState = MSG2SSP_AP_STATUS_SUSPEND;
	disable_debug_timer(data);

	data->bTimeSyncing = false;
	return 0;
}

static int ssp_resume(struct device *dev)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	func_dbg();
	enable_debug_timer(data);

	if (data->bMcuReady &&
	    SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_RESUME, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_RESUME failed\n",
			__func__);
	data->uLastResumeState = MSG2SSP_AP_STATUS_RESUME;

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ssp_pm_ops, ssp_suspend, ssp_resume);

static const struct platform_device_id ssp_id[] = {
	{ "bbd-ssp", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, ssp_id);

static struct platform_driver ssp_driver = {
	.probe = ssp_probe,
	.remove = ssp_remove,
	.shutdown = ssp_shutdown,
	.id_table = ssp_id,
	.driver = {
		.name = "bbd-ssp",
		.pm = pm_sleep_ptr(&ssp_pm_ops),
	},
};
module_platform_driver(ssp_driver);

MODULE_DESCRIPTION("Samsung SSP sensor hub on Broadcom BCM4773 (BBD)");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
/* industrialio is =m and Module.symvers is missing, so modpost cannot record it */
MODULE_SOFTDEP("pre: industrialio");
