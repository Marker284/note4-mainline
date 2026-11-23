/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Port: the enable/delay state machine of downstream ssp_sysfs.c, now
 *  driven by ssp_iio.c.  The Samsung sysfs ABI (/sys/class/sensors/
 *  ssp_sensor/{enable,*_poll_delay,...}, batch ioctl misc device) is
 *  reduced to a few attributes on the bbd-ssp platform device:
 *    enable       sensor bitmask (same semantics as downstream "enable")
 *    enable_irq   1 = reset MCU and resume, 0 = shut SSP down
 *    mcu_rev      firmware revision reported by the hub
 *    mcu_reset    read triggers a reset request to LHD
 *    sensor_state bitmask of sensors present on the hub
 *    ready        1 once the MCU has been initialised after ESW:READY
 */
#include "ssp.h"
#include <linux/math64.h>

/*************************************************************************/
/* SSP data delay function                                              */
/*************************************************************************/

int get_msdelay(int64_t dDelayRate)
{
	/*
	 * From Android 5.0, There is MaxDelay Concept.
	 * If App request lower frequency then MaxDelay,
	 * Sensor have to work with MaxDelay.
	 */

	if (dDelayRate > 200000000)
		dDelayRate = 200000000;

	return div_s64(dDelayRate, 1000000);
}

static void enable_sensor(struct ssp_data *data,
	int iSensorType, int64_t dNewDelay)
{
	u8 uBuf[9];
	unsigned int uNewEnable = 0;
	s32 maxBatchReportLatency = 0;
	s8 batchOptions = 0;
	int64_t dTempDelay = data->adDelayBuf[iSensorType];
	s32 dMsDelay = get_msdelay(dNewDelay);
	int ret = 0;

	data->adDelayBuf[iSensorType] = dNewDelay;
	maxBatchReportLatency = data->batchLatencyBuf[iSensorType];
	batchOptions = data->batchOptBuf[iSensorType];

	switch (data->aiCheckStatus[iSensorType]) {
	case ADD_SENSOR_STATE:
		ssp_dbg_print("[SSP]: %s - add %u, New = %lldns\n",
			 __func__, 1 << iSensorType, dNewDelay);

		if (iSensorType == PROXIMITY_SENSOR) {
			get_proximity_threshold(data);
			proximity_open_calibration(data);
			set_proximity_threshold(data, data->uProxHiThresh,
				data->uProxLoThresh);
		}

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;

		ret = send_instruction(data, ADD_SENSOR, iSensorType, uBuf, 9);
		pr_info("[SSP] add sensor %d, delay %d, timeout %d, flag=%d, ret %d\n",
			iSensorType, dMsDelay, maxBatchReportLatency, uBuf[8], ret);
		if (ret <= 0) {
			uNewEnable =
				(unsigned int)atomic_read(&data->aSensorEnable)
				& (~(unsigned int)(1 << iSensorType));
			atomic_set(&data->aSensorEnable, uNewEnable);

			data->aiCheckStatus[iSensorType] = NO_SENSOR_STATE;
			break;
		}

		data->aiCheckStatus[iSensorType] = RUNNING_SENSOR_STATE;

		break;
	case RUNNING_SENSOR_STATE:
		if (get_msdelay(dTempDelay)
			== get_msdelay(data->adDelayBuf[iSensorType]))
			break;

		ssp_dbg_print("[SSP]: %s - Change %u, New = %lldns\n",
			__func__, 1 << iSensorType, dNewDelay);

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;
		send_instruction(data, CHANGE_DELAY, iSensorType, uBuf, 9);

		break;
	default:
		data->aiCheckStatus[iSensorType] = ADD_SENSOR_STATE;
	}
}

static void change_sensor_delay(struct ssp_data *data,
	int iSensorType, int64_t dNewDelay)
{
	u8 uBuf[9];
	s32 maxBatchReportLatency = 0;
	s8 batchOptions = 0;
	int64_t dTempDelay = data->adDelayBuf[iSensorType];
	s32 dMsDelay = get_msdelay(dNewDelay);

	data->adDelayBuf[iSensorType] = dNewDelay;
	data->batchLatencyBuf[iSensorType] = maxBatchReportLatency;
	data->batchOptBuf[iSensorType] = batchOptions;

	switch (data->aiCheckStatus[iSensorType]) {
	case RUNNING_SENSOR_STATE:
		if (get_msdelay(dTempDelay)
			== get_msdelay(data->adDelayBuf[iSensorType]))
			break;

		ssp_dbg_print("[SSP]: %s - Change %u, New = %lldns\n",
			__func__, 1 << iSensorType, dNewDelay);

		memcpy(&uBuf[0], &dMsDelay, 4);
		memcpy(&uBuf[4], &maxBatchReportLatency, 4);
		uBuf[8] = batchOptions;
		send_instruction(data, CHANGE_DELAY, iSensorType, uBuf, 9);

		break;
	default:
		break;
	}
}

static int ssp_remove_sensor(struct ssp_data *data,
	unsigned int uChangedSensor, unsigned int uNewEnable)
{
	u8 uBuf[4];
	int64_t dSensorDelay = data->adDelayBuf[uChangedSensor];

	ssp_dbg_print("[SSP]: %s - remove sensor = %d, current state = %d\n",
		__func__, (1 << uChangedSensor), uNewEnable);

	/* Port: keep the delay chosen through IIO across disable/enable */
	data->batchLatencyBuf[uChangedSensor] = 0;
	data->batchOptBuf[uChangedSensor] = 0;

	if (!data->bSspShutdown)
		if (atomic_read(&data->aSensorEnable) & (1 << uChangedSensor)) {
			s32 dMsDelay = get_msdelay(dSensorDelay);
			memcpy(&uBuf[0], &dMsDelay, 4);

			send_instruction(data, REMOVE_SENSOR,
				uChangedSensor, uBuf, 4);
		}
	data->aiCheckStatus[uChangedSensor] = NO_SENSOR_STATE;

	return 0;
}

/*
 * Same logic as downstream set_sensors_enable(), for one sensor.
 * Returns 0, -ENODEV if the hub does not have that sensor.
 */
int ssp_set_sensor_enable(struct ssp_data *data, unsigned int type, bool on)
{
	unsigned int cur, uNewEnable;
	int iRet;

	if (type >= SENSOR_MAX)
		return -EINVAL;

	mutex_lock(&data->enable_mutex);
	cur = atomic_read(&data->aSensorEnable);
	if (!!(cur & (1 << type)) == on) {
		mutex_unlock(&data->enable_mutex);
		return 0;
	}
	if (on && data->bMcuReady && !(data->uSensorState & (1 << type))) {
		mutex_unlock(&data->enable_mutex);
		pr_info("[SSP] %s - sensor %u is not connected (0x%x)\n",
			__func__, type, data->uSensorState);
		return -ENODEV;
	}

	uNewEnable = on ? (cur | (1 << type)) : (cur & ~(1 << type));

	if (!on) {
		data->reportedData[type] = false;
		ssp_remove_sensor(data, type, uNewEnable); /* disable */
	} else if (!data->bSspShutdown) {
		/* Change to ADD_SENSOR_STATE from KitKat */
		if (data->aiCheckStatus[type] == INITIALIZATION_STATE) {
			if (type == ACCELEROMETER_SENSOR) {
				accel_open_calibration(data);
				iRet = set_accel_cal(data);
				if (iRet < 0)
					pr_err("[SSP]: %s - set_accel_cal failed %d\n", __func__, iRet);
			} else if (type == GYROSCOPE_SENSOR) {
				gyro_open_calibration(data);
				iRet = set_gyro_cal(data);
				if (iRet < 0)
					pr_err("[SSP]: %s - set_gyro_cal failed %d\n", __func__, iRet);
			} else if (type == PRESSURE_SENSOR) {
				pressure_open_calibration(data);
			} else if (type == PROXIMITY_SENSOR) {
				get_proximity_threshold(data);
				proximity_open_calibration(data);
				set_proximity_threshold(data, data->uProxHiThresh, data->uProxLoThresh);
			}
		}
		data->aiCheckStatus[type] = ADD_SENSOR_STATE;
		enable_sensor(data, type, data->adDelayBuf[type]);
		/* enable_sensor() clears the bit again on failure */
		if (data->aiCheckStatus[type] == NO_SENSOR_STATE)
			uNewEnable &= ~(1 << type);
	}
	/* while the MCU is down the bit is only recorded; sync_sensor_state()
	 * adds the sensor once ESW:READY arrives */
	atomic_set(&data->aSensorEnable, uNewEnable);
	mutex_unlock(&data->enable_mutex);

	return 0;
}

void ssp_set_sensor_delay(struct ssp_data *data, unsigned int type,
			  int64_t delay_ns)
{
	if (type >= SENSOR_MAX)
		return;
	mutex_lock(&data->enable_mutex);
	change_sensor_delay(data, type, delay_ns);
	mutex_unlock(&data->enable_mutex);
}

/*************************************************************************/
/* ssp Sysfs                                                             */
/*************************************************************************/

static ssize_t enable_irq_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", !data->bSspShutdown);
}

static ssize_t enable_irq_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	u8 dTemp;
	struct ssp_data *data = dev_get_drvdata(dev);

	if (kstrtou8(buf, 10, &dTemp) < 0)
		return -EINVAL;

	pr_info("[SSP] %s - %d start\n", __func__, dTemp);
	if (dTemp) {
		reset_mcu(data);
		enable_debug_timer(data);
	} else {
		disable_debug_timer(data);
		ssp_enable(data, 0);
	}
	return size;
}
static DEVICE_ATTR_RW(enable_irq);

static ssize_t enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%9u\n", atomic_read(&data->aSensorEnable));
}

static ssize_t enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct ssp_data *data = dev_get_drvdata(dev);
	unsigned int uNewEnable, old, i;
	int ret;

	if (kstrtouint(buf, 10, &uNewEnable) < 0)
		return -EINVAL;

	old = atomic_read(&data->aSensorEnable);
	for (i = 0; i < SENSOR_MAX; i++) {
		if (((old ^ uNewEnable) >> i) & 1) {
			ret = ssp_set_sensor_enable(data, i, (uNewEnable >> i) & 1);
			if (ret)
				return ret;
		}
	}
	return size;
}
static DEVICE_ATTR_RW(enable);

static ssize_t mcu_rev_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "BR01%u,BR01%u\n", get_module_rev(data),
			  data->uCurFirmRev);
}
static DEVICE_ATTR_RO(mcu_rev);

static ssize_t mcu_reset_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	reset_mcu(data);
	return sysfs_emit(buf, "OK\n");
}
static DEVICE_ATTR_RO(mcu_reset);

static ssize_t sensor_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%x\n", data->uSensorState);
}
static DEVICE_ATTR_RO(sensor_state);

static ssize_t ready_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", data->bMcuReady);
}
static DEVICE_ATTR_RO(ready);

static ssize_t stats_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "irq=%u reset=%u comfail=%u timeout=%u\n",
			  data->uIrqCnt, data->uResetCnt, data->uComFailCnt,
			  data->uTimeOutCnt);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *ssp_attrs[] = {
	&dev_attr_enable_irq.attr,
	&dev_attr_enable.attr,
	&dev_attr_mcu_rev.attr,
	&dev_attr_mcu_reset.attr,
	&dev_attr_sensor_state.attr,
	&dev_attr_ready.attr,
	&dev_attr_stats.attr,
	NULL,
};

static const struct attribute_group ssp_attr_group = {
	.attrs = ssp_attrs,
};

int initialize_sysfs(struct ssp_data *data)
{
	return sysfs_create_group(&data->dev->kobj, &ssp_attr_group);
}

void remove_sysfs(struct ssp_data *data)
{
	sysfs_remove_group(&data->dev->kobj, &ssp_attr_group);
}
