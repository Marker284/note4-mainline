/*
 *  Copyright (C) 2011, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Linux 7.3 port (BBD/BCM4773 variant only, module ssp_bbd.ko):
 *   - input devices, Samsung factory sysfs, sensorhub library channel,
 *     big-data/dump tasks, mobeam, temp/humidity, early-suspend dropped
 *   - wake_lock -> pm_wakeup_event, setup_timer -> timer_setup
 *   - IIO front end rewritten (ssp_iio.c) for iio-sensor-proxy
 */

#ifndef __SSP_PRJ_H__
#define __SSP_PRJ_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include "bbdpl/bbd.h"

#define SSP_DBG		1

#define SUCCESS		1
#define FAIL		0
#define ERROR		-1

#define FACTORY_DATA_MAX	99

/* ssp mcu device ID */
#define DEVICE_ID			0x55

#define ssp_dbg_print(format, ...) do { \
	if (unlikely(ssp_dbg)) \
		pr_info(format, ##__VA_ARGS__); \
	} while (0)

#define func_dbg() ssp_dbg_print("[SSP]: %s\n", __func__)
#define data_dbg(format, ...) do { } while (0)

#define SSP_SW_RESET_TIME	3000
#define DEFUALT_POLLING_DELAY	(200 * NSEC_PER_MSEC)
#define PROX_AVG_READ_NUM	80
#define DEFAULT_RETRIES		3
#define DATA_PACKET_SIZE	960

#define SSP_DEBUG_TIMER_SEC	(10 * HZ)
#define LIMIT_RESET_CNT		20
#define LIMIT_TIMEOUT_CNT	3

enum {
	INITIALIZATION_STATE = 0,
	NO_SENSOR_STATE,
	ADD_SENSOR_STATE,
	RUNNING_SENSOR_STATE,
};

#define SSP_INVALID_REVISION		99999
#define SSP_INVALID_REVISION2		0xFFFFFF

/* Gyroscope DPS */
#define GYROSCOPE_DPS250		250
#define GYROSCOPE_DPS500		500
#define GYROSCOPE_DPS2000		2000

/* AP -> SSP Instruction */
#define MSG2SSP_INST_BYPASS_SENSOR_ADD	0xA1
#define MSG2SSP_INST_BYPASS_SENSOR_REMOVE	0xA2
#define MSG2SSP_INST_REMOVE_ALL		0xA3
#define MSG2SSP_INST_CHANGE_DELAY		0xA4
#define MSG2SSP_INST_LIBRARY_ADD		0xB1
#define MSG2SSP_INST_LIBRARY_REMOVE		0xB2
#define MSG2SSP_INST_LIB_NOTI		0xB4
#define MSG2SSP_INST_LIB_DATA		0xC1

#define MSG2SSP_AP_MCU_SET_GYRO_CAL		0xCD
#define MSG2SSP_AP_MCU_SET_ACCEL_CAL		0xCE
#define MSG2SSP_AP_STATUS_SHUTDOWN		0xD0
#define MSG2SSP_AP_STATUS_WAKEUP		0xD1
#define MSG2SSP_AP_STATUS_SLEEP		0xD2
#define MSG2SSP_AP_STATUS_RESUME		0xD3
#define MSG2SSP_AP_STATUS_SUSPEND		0xD4
#define MSG2SSP_AP_STATUS_RESET		0xD5
#define MSG2SSP_AP_STATUS_POW_CONNECTED	0xD6
#define MSG2SSP_AP_STATUS_POW_DISCONNECTED	0xD7
#define MSG2SSP_AP_TEMPHUMIDITY_CAL_DONE	0xDA
#define MSG2SSP_AP_MCU_SET_DUMPMODE		0xDB
#define MSG2SSP_AP_MCU_DUMP_CHECK		0xDC
#define MSG2SSP_AP_MCU_BATCH_FLUSH		0xDD
#define MSG2SSP_AP_MCU_BATCH_COUNT		0xDF

#define MSG2SSP_AP_WHOAMI			0x0F
#define MSG2SSP_AP_FIRMWARE_REV		0xF0
#define MSG2SSP_AP_SENSOR_FORMATION		0xF1
#define MSG2SSP_AP_SENSOR_PROXTHRESHOLD	0xF2
#define MSG2SSP_AP_SENSOR_BARCODE_EMUL	0xF3
#define MSG2SSP_AP_SENSOR_SCANNING		0xF4
#define MSG2SSP_AP_SET_MAGNETIC_HWOFFSET	0xF5
#define MSG2SSP_AP_GET_MAGNETIC_HWOFFSET	0xF6
#define MSG2SSP_AP_SENSOR_GESTURE_CURRENT	0xF7
#define MSG2SSP_AP_GET_THERM		0xF8
#define MSG2SSP_AP_GET_BIG_DATA		0xF9
#define MSG2SSP_AP_SET_BIG_DATA		0xFA
#define MSG2SSP_AP_START_BIG_DATA		0xFB
#define MSG2SSP_AP_SET_MAGNETIC_STATIC_MATRIX	0xFD
#define MSG2SSP_AP_SENSOR_TILT			0xEA
#define MSG2SSP_AP_MCU_SET_TIME			0xFE
#define MSG2SSP_AP_MCU_GET_TIME			0xFF

#define DEFUALT_HIGH_THRESHOLD		185
#define DEFUALT_LOW_THRESHOLD		145

/* SSP -> AP ACK about write CMD */
#define MSG_ACK		0x80	/* ACK from SSP to AP */
#define MSG_NAK		0x70	/* NAK from SSP to AP */

/* SSP_INSTRUCTION_CMD */
enum {
	REMOVE_SENSOR = 0,
	ADD_SENSOR,
	CHANGE_DELAY,
	GO_SLEEP,
	REMOVE_LIBRARY,
	ADD_LIBRARY,
};

/* SENSOR_TYPE (hub numbering, do not change) */
enum {
	ACCELEROMETER_SENSOR = 0,
	GYROSCOPE_SENSOR,
	GEOMAGNETIC_UNCALIB_SENSOR,
	GEOMAGNETIC_RAW,
	GEOMAGNETIC_SENSOR,
	PRESSURE_SENSOR,
	GESTURE_SENSOR,
	PROXIMITY_SENSOR,
	TEMPERATURE_HUMIDITY_SENSOR,
	LIGHT_SENSOR,
	PROXIMITY_RAW,
	ORIENTATION_SENSOR,
	STEP_DETECTOR = 12,
	SIG_MOTION_SENSOR,
	GYRO_UNCALIB_SENSOR,
	GAME_ROTATION_VECTOR = 15,
	ROTATION_VECTOR,
	STEP_COUNTER,
	BIO_HRM_RAW,
	BIO_HRM_RAW_FAC,
	BIO_HRM_LIB,
	SHAKE_CAM = 22,
	SENSOR_MAX,
};

struct meta_data_event {
	s32 what;
	s32 sensor;
} __packed;

/*
 * Data layout as sent by the hub.  Note 4 (trelte) has TMG3992 light
 * (CONFIG_SENSORS_SSP_TMG399X: r,g,b,w + a_time + a_gain = 10 bytes).
 */
struct sensor_value {
	union {
		struct {
			s16 x;
			s16 y;
			s16 z;
		};
		struct {		/*calibrated mag, gyro*/
			s16 cal_x;
			s16 cal_y;
			s16 cal_z;
			u8 accuracy;
		};
		struct {		/*uncalibrated mag, gyro*/
			s16 uncal_x;
			s16 uncal_y;
			s16 uncal_z;
			s16 offset_x;
			s16 offset_y;
			s16 offset_z;
		};
		struct {		/* rotation vector */
			s32 quat_a;
			s32 quat_b;
			s32 quat_c;
			s32 quat_d;
			u8 acc_rot;
		};
		struct {
			u16 r;
			u16 g;
			u16 b;
			u16 w;
			u8 a_time;
			u8 a_gain;
		};
		u8 step_det;
		u8 sig_motion;
		u8 prox[4];
		u8 data[20];
		s32 pressure[3];
		u32 step_diff;
		struct meta_data_event meta_data;
	};
	u64 timestamp;
} __packed;

struct calibraion_data {
	s16 x;
	s16 y;
	s16 z;
};

struct hw_offset_data {
	char x;
	char y;
	char z;
};

/* ssp_msg options bit*/
#define SSP_SPI		0	/* read write mask */
#define SSP_RETURN	2	/* write and read option */
#define SSP_GYRO_DPS	3	/* gyro dps mask */
#define SSP_INDEX	3	/* data index mask */

#define SSP_SPI_MASK		(3 << SSP_SPI)	/* read write mask */
#define SSP_GYRO_DPS_MASK	(3 << SSP_GYRO_DPS)
#define SSP_INDEX_MASK		(8191 << SSP_INDEX)

/*
 * The first 9 bytes (cmd, length, options, data; little endian, packed)
 * are sent to the hub as the request header.  Do not reorder.
 */
struct ssp_msg {
	u8 cmd;
	u16 length;
	u16 options;
	u32 data;

	struct list_head list;
	struct completion *done;
	char *buffer;
	u8 free_buffer;
	bool *dead_hook;
	bool dead;
} __packed;

#define SSP_MSG_HEADER_SIZE	9

enum {
	AP2HUB_READ = 0,
	AP2HUB_WRITE,
	HUB2AP_WRITE,
	AP2HUB_READY,
	AP2HUB_RETURN
};

enum {
	BIG_TYPE_DUMP = 0,
	BIG_TYPE_READ_LIB,
	BIG_TYPE_VOICE_NET,
	BIG_TYPE_VOICE_GRAM,
	BIG_TYPE_VOICE_PCM,
	BIG_TYPE_TEMP,
	BIG_TYPE_MAX,
};

enum {
	BATCH_MODE_NONE = 0,
	BATCH_MODE_RUN,
};

struct ssp_time_diff {
	u16 batch_count;
	u16 batch_mode;
	u64 time_diff;
	u64 irq_diff;
	u16 batch_count_fixed;
};

/* IIO sensors exposed by ssp_iio.c */
enum ssp_iio_sensor {
	SSP_IIO_ACCEL = 0,
	SSP_IIO_GYRO,
	SSP_IIO_MAGN,
	SSP_IIO_LIGHT,
	SSP_IIO_PROX,
	SSP_IIO_PRESSURE,
	SSP_IIO_MAX,
};

struct ssp_data {
	struct device *dev;			/* bbd-ssp platform device */
	struct iio_dev *indio[SSP_IIO_MAX];

	struct workqueue_struct *bbd_on_packet_wq;
	struct work_struct work_bbd_on_packet;
	struct workqueue_struct *bbd_mcu_ready_wq;
	struct work_struct work_bbd_mcu_ready;

	struct timer_list debug_timer;
	struct workqueue_struct *debug_wq;
	struct work_struct work_debug;
	struct calibraion_data accelcal;
	struct calibraion_data gyrocal;
	struct hw_offset_data magoffset;
	struct sensor_value buf[SENSOR_MAX];

	/* new data notification for the IIO raw reads */
	wait_queue_head_t data_wq;
	unsigned int data_seq[SENSOR_MAX];

	bool bSspShutdown;
	bool bProximityRawEnabled;
	bool bMcuDumpMode;
	bool bProbeIsDone;
	bool bTimeSyncing;
	bool bHandlingIrq;
	bool bMcuReady;

	unsigned int uProxCanc;
	unsigned int uProxHiThresh;
	unsigned int uProxLoThresh;
	unsigned int uProxHiThresh_default;
	unsigned int uProxLoThresh_default;

	unsigned int uComFailCnt;
	unsigned int uResetCnt;
	unsigned int uTimeOutCnt;
	unsigned int uIrqCnt;

	unsigned int uGyroDps;
	unsigned int uSensorState;
	unsigned int uCurFirmRev;
	char uLastResumeState;
	char uLastAPState;
	s32 iPressureCal;
	u64 step_count_total;

	atomic_t aSensorEnable;
	int64_t adDelayBuf[SENSOR_MAX];
	u64 lastTimestamp[SENSOR_MAX];
	s32 batchLatencyBuf[SENSOR_MAX];
	s8 batchOptBuf[SENSOR_MAX];
	bool reportedData[SENSOR_MAX];
	int aiCheckStatus[SENSOR_MAX];

	void (*get_sensor_data[SENSOR_MAX])(char *, int *,
		struct sensor_value *);
	void (*report_sensor_data[SENSOR_MAX])(struct ssp_data *,
		struct sensor_value *);

	u32 ap_rev;
	u32 accel_position;
	u32 mag_position;
	s16 *static_matrix;		/* ssp-mag-array (YAS532) */

	struct mutex comm_mutex;
	struct mutex pending_mutex;
	struct mutex enable_mutex;

	struct list_head pending_list;
	u64 timestamp;

	unsigned char rBuff[BBD_MAX_DATA_SIZE];	/* bbd_on_packet_work_func */
};

/* ssp_dev.c */
void ssp_enable(struct ssp_data *, bool);
int initialize_mcu(struct ssp_data *);

/* ssp_bbd.c */
int bbd_do_transfer(struct ssp_data *data, struct ssp_msg *msg,
		struct completion *done, int timeout);
int callback_bbd_on_control(void *ssh_data, char *str_ctrl);
int callback_bbd_on_mcu_ready(void *ssh_data, bool ready);
int callback_bbd_on_packet_alarm(void *ssh_data);
void bbd_on_packet_work_func(struct work_struct *work);
void bbd_mcu_ready_work_func(struct work_struct *work);

/* ssp_i2c.c (command layer) */
void clean_msg(struct ssp_msg *msg);
int ssp_spi_async(struct ssp_data *, struct ssp_msg *);
int ssp_spi_sync(struct ssp_data *, struct ssp_msg *, int);
void clean_pending_list(struct ssp_data *);
int ssp_send_cmd(struct ssp_data *, char, int);
int send_instruction(struct ssp_data *, u8, u8, u8 *, u8);
int send_instruction_sync(struct ssp_data *, u8, u8, u8 *, u8);
int flush(struct ssp_data *, u8);
int get_batch_count(struct ssp_data *, u8);
int get_chipid(struct ssp_data *);
int set_sensor_position(struct ssp_data *);
void set_proximity_threshold(struct ssp_data *, unsigned int, unsigned int);
unsigned int get_sensor_scanning_info(struct ssp_data *);
unsigned int get_firmware_rev(struct ssp_data *);
int set_time(struct ssp_data *);
int get_time(struct ssp_data *);

/* ssp_calib.c (from the downstream factory/ sources) */
int set_gyro_cal(struct ssp_data *);
int set_accel_cal(struct ssp_data *);
int initialize_magnetic_sensor(struct ssp_data *data);
void get_proximity_threshold(struct ssp_data *);
int proximity_open_calibration(struct ssp_data *);
int accel_open_calibration(struct ssp_data *);
int gyro_open_calibration(struct ssp_data *);
int pressure_open_calibration(struct ssp_data *);

/* ssp_data.c */
int parse_dataframe(struct ssp_data *, char *, int);
void initialize_function_pointer(struct ssp_data *);

/* ssp_sysfs.c */
int get_msdelay(int64_t);
int ssp_set_sensor_enable(struct ssp_data *data, unsigned int type, bool on);
void ssp_set_sensor_delay(struct ssp_data *data, unsigned int type,
			  int64_t delay_ns);
int initialize_sysfs(struct ssp_data *);
void remove_sysfs(struct ssp_data *);

/* ssp_debug.c */
int print_mcu_debug(char *, int *, int);
void reset_mcu(struct ssp_data *);
void sync_sensor_state(struct ssp_data *);
void enable_debug_timer(struct ssp_data *);
void disable_debug_timer(struct ssp_data *);
int initialize_debug_timer(struct ssp_data *);
unsigned int ssp_check_sec_dump_mode(void);
unsigned int get_module_rev(struct ssp_data *data);

/* ssp_iio.c */
int ssp_iio_init(struct ssp_data *data);
void ssp_iio_remove(struct ssp_data *data);
void report_meta_data(struct ssp_data *, struct sensor_value *);
void report_acc_data(struct ssp_data *, struct sensor_value *);
void report_gyro_data(struct ssp_data *, struct sensor_value *);
void report_mag_data(struct ssp_data *, struct sensor_value *);
void report_mag_uncaldata(struct ssp_data *, struct sensor_value *);
void report_rot_data(struct ssp_data *, struct sensor_value *);
void report_game_rot_data(struct ssp_data *, struct sensor_value *);
void report_step_det_data(struct ssp_data *, struct sensor_value *);
void report_gesture_data(struct ssp_data *, struct sensor_value *);
void report_pressure_data(struct ssp_data *, struct sensor_value *);
void report_light_data(struct ssp_data *, struct sensor_value *);
void report_prox_data(struct ssp_data *, struct sensor_value *);
void report_prox_raw_data(struct ssp_data *, struct sensor_value *);
void report_geomagnetic_raw_data(struct ssp_data *, struct sensor_value *);
void report_sig_motion_data(struct ssp_data *, struct sensor_value *);
void report_uncalib_gyro_data(struct ssp_data *, struct sensor_value *);
void report_step_cnt_data(struct ssp_data *, struct sensor_value *);
void report_temp_humidity_data(struct ssp_data *, struct sensor_value *);
void report_shake_cam_data(struct ssp_data *, struct sensor_value *);

#endif
