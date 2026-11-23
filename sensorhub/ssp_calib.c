/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Calibration helpers taken from downstream factory/{accel,gyro}_icm20610.c,
 *  factory/magnetic_yas532.c, factory/prox_tmg399x.c and
 *  factory/pressure_bmp182.c.  The factory sysfs nodes are dropped, and so
 *  is reading calibration from /efs/FactoryApp files with filp_open(): the
 *  offsets start at zero (TODO: optional DT/module-param calibration).
 */
#include "ssp.h"

/* factory/magnetic_yas532.h */
#define YAS_STATIC_ELLIPSOID_MATRIX	{10000, 0, 0, 0, 10000, 0, 0, 0, 10000}

int accel_open_calibration(struct ssp_data *data)
{
	return 0;
}

int gyro_open_calibration(struct ssp_data *data)
{
	return 0;
}

int pressure_open_calibration(struct ssp_data *data)
{
	data->iPressureCal = 0;
	return 0;
}

int proximity_open_calibration(struct ssp_data *data)
{
	data->uProxCanc = 0;
	return 0;
}

void get_proximity_threshold(struct ssp_data *data)
{
	data->uProxHiThresh = data->uProxHiThresh_default;
	data->uProxLoThresh = data->uProxLoThresh_default;
}

static int ssp_send_cal(struct ssp_data *data, u8 cmd, const void *buf, u16 len)
{
	struct ssp_msg *msg;
	int iRet;

	msg = kzalloc(sizeof(*msg), GFP_KERNEL);
	if (msg == NULL)
		return -ENOMEM;
	msg->cmd = cmd;
	msg->length = len;
	msg->options = AP2HUB_WRITE;
	msg->buffer = kmemdup(buf, len, GFP_KERNEL);
	if (!msg->buffer) {
		kfree(msg);
		return -ENOMEM;
	}
	msg->free_buffer = 1;

	iRet = ssp_spi_async(data, msg);
	if (iRet != SUCCESS) {
		pr_err("[SSP]: %s - cmd 0x%02x fail %d\n", __func__, cmd, iRet);
		iRet = ERROR;
	}
	return iRet;
}

int set_accel_cal(struct ssp_data *data)
{
	s16 accel_cal[3];

	if (!(data->uSensorState & (1 << ACCELEROMETER_SENSOR)))
		return 0;

	accel_cal[0] = data->accelcal.x;
	accel_cal[1] = data->accelcal.y;
	accel_cal[2] = data->accelcal.z;

	return ssp_send_cal(data, MSG2SSP_AP_MCU_SET_ACCEL_CAL, accel_cal, 6);
}

int set_gyro_cal(struct ssp_data *data)
{
	s16 gyro_cal[3];

	if (!(data->uSensorState & (1 << GYROSCOPE_SENSOR)))
		return 0;

	gyro_cal[0] = data->gyrocal.x;
	gyro_cal[1] = data->gyrocal.y;
	gyro_cal[2] = data->gyrocal.z;

	return ssp_send_cal(data, MSG2SSP_AP_MCU_SET_GYRO_CAL, gyro_cal, 6);
}

/* YAS532 soft-iron matrix (DT "ssp-mag-array", 9 x s16) */
static int set_static_matrix(struct ssp_data *data)
{
	static const s16 def_matrix[9] = YAS_STATIC_ELLIPSOID_MATRIX;

	if (!(data->uSensorState & 0x04))
		return 0;

	return ssp_send_cal(data, MSG2SSP_AP_SET_MAGNETIC_STATIC_MATRIX,
			    data->static_matrix ? data->static_matrix : def_matrix,
			    18);
}

int initialize_magnetic_sensor(struct ssp_data *data)
{
	int ret = set_static_matrix(data);

	if (ret < 0)
		pr_err("[SSP]: %s - set_magnetic_static_matrix failed %d\n",
			__func__, ret);
	return ret;
}
