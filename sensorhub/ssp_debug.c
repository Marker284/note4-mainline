/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Port of ssp_debug.c + ssp_firmware.c: debug timer (timer_setup),
 *  MCU reset via LHD, state resync.  MCU dump / big-data tasks dropped.
 */
#include "ssp.h"

#define SSP_FIRMWARE_REVISION_BCM	16030700

unsigned int get_module_rev(struct ssp_data *data)
{
	return SSP_FIRMWARE_REVISION_BCM;
}

/*************************************************************************/
/* SSP Debug timer function                                              */
/*************************************************************************/
int print_mcu_debug(char *pchRcvDataFrame, int *pDataIdx,
		int iRcvDataFrameLength)
{
	int iLength = pchRcvDataFrame[(*pDataIdx)++];
	int cur = *pDataIdx;

	if (iLength > iRcvDataFrameLength - *pDataIdx || iLength <= 0) {
		ssp_dbg_print("[SSP]: MSG From MCU - invalid debug length(%d/%d/%d)\n",
			iLength, iRcvDataFrameLength, cur);
		return iLength ? iLength : ERROR;
	}

	ssp_dbg_print("[SSP]: MSG From MCU - %.*s\n", iLength,
		      &pchRcvDataFrame[*pDataIdx]);
	*pDataIdx += iLength;
	return 0;
}

void reset_mcu(struct ssp_data *data)
{
	func_dbg();
	data->uResetCnt++;

	data->bMcuReady = false;
	ssp_enable(data, false);
	clean_pending_list(data);

	/* asks LHD (via /dev/bbd_control "BBD:RESET_REQ") to restart the ESW */
	bbd_mcu_reset();
}

void sync_sensor_state(struct ssp_data *data)
{
	unsigned char uBuf[9] = {0,};
	unsigned int uSensorCnt;
	int iRet = 0;

	iRet = set_gyro_cal(data);
	if (iRet < 0)
		pr_err("[SSP]: %s - set_gyro_cal failed\n", __func__);

	iRet = set_accel_cal(data);
	if (iRet < 0)
		pr_err("[SSP]: %s - set_accel_cal failed\n", __func__);

	udelay(10);

	for (uSensorCnt = 0; uSensorCnt < SENSOR_MAX; uSensorCnt++) {
		mutex_lock(&data->enable_mutex);
		if (atomic_read(&data->aSensorEnable) & (1 << uSensorCnt)) {
			s32 dMsDelay =
				get_msdelay(data->adDelayBuf[uSensorCnt]);
			memcpy(&uBuf[0], &dMsDelay, 4);
			memcpy(&uBuf[4], &data->batchLatencyBuf[uSensorCnt], 4);
			uBuf[8] = data->batchOptBuf[uSensorCnt];
			if (send_instruction(data, ADD_SENSOR, uSensorCnt, uBuf, 9) > 0)
				data->aiCheckStatus[uSensorCnt] = RUNNING_SENSOR_STATE;
			udelay(10);
		}
		mutex_unlock(&data->enable_mutex);
	}

	if (data->bProximityRawEnabled == true) {
		s32 dMsDelay = 20;
		memcpy(&uBuf[0], &dMsDelay, 4);
		send_instruction(data, ADD_SENSOR, PROXIMITY_RAW, uBuf, 4);
	}

	set_proximity_threshold(data, data->uProxHiThresh, data->uProxLoThresh);

	data->bMcuDumpMode = ssp_check_sec_dump_mode();
	iRet = ssp_send_cmd(data, MSG2SSP_AP_MCU_SET_DUMPMODE,
		data->bMcuDumpMode);
	if (iRet < 0)
		pr_err("[SSP]: %s - MSG2SSP_AP_MCU_SET_DUMPMODE failed\n",
			__func__);
}

static void debug_work_func(struct work_struct *work)
{
	struct ssp_data *data = container_of(work, struct ssp_data, work_debug);

	ssp_dbg_print("[SSP]: %s(%u) - Sensor state: 0x%x, RC: %u, CC: %u, TC: %u\n",
		__func__, data->uIrqCnt, data->uSensorState, data->uResetCnt,
		data->uComFailCnt, data->uTimeOutCnt);

	if (data->bMcuReady &&
	    (((atomic_read(&data->aSensorEnable) & (1 << ACCELEROMETER_SENSOR))
	      && (data->batchLatencyBuf[ACCELEROMETER_SENSOR] == 0)
	      && (data->uIrqCnt == 0) && (data->uTimeOutCnt > 0))
	     || (data->uTimeOutCnt > LIMIT_TIMEOUT_CNT))) {

		if (data->uResetCnt < LIMIT_RESET_CNT) {
			pr_info("[SSP] : %s - uTimeOutCnt(%u), pending(%u)\n",
				__func__, data->uTimeOutCnt,
				!list_empty(&data->pending_list));
			reset_mcu(data);
		} else
			ssp_enable(data, false);

		data->uTimeOutCnt = 0;
		data->uComFailCnt = 0;
	}

	data->uIrqCnt = 0;
}

static void debug_timer_func(struct timer_list *t)
{
	struct ssp_data *data = timer_container_of(data, t, debug_timer);

	queue_work(data->debug_wq, &data->work_debug);
	mod_timer(&data->debug_timer,
		round_jiffies_up(jiffies + SSP_DEBUG_TIMER_SEC));
}

void enable_debug_timer(struct ssp_data *data)
{
	mod_timer(&data->debug_timer,
		round_jiffies_up(jiffies + SSP_DEBUG_TIMER_SEC));
}

void disable_debug_timer(struct ssp_data *data)
{
	timer_delete_sync(&data->debug_timer);
	cancel_work_sync(&data->work_debug);
}

int initialize_debug_timer(struct ssp_data *data)
{
	timer_setup(&data->debug_timer, debug_timer_func, 0);

	data->debug_wq = alloc_ordered_workqueue("ssp_debug_wq", 0);
	if (!data->debug_wq)
		return ERROR;

	INIT_WORK(&data->work_debug, debug_work_func);
	return SUCCESS;
}

unsigned int ssp_check_sec_dump_mode(void)
{
	return 0;
}
