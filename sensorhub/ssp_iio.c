// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IIO front end for the SSP sensor hub on BCM4773 (Note 4 Exynos).
 *
 * Replaces downstream ssp_input.c / ssp_iio_ring.c / ssp_iio_trigger.c,
 * which pushed into Android-specific IIO buffers and input devices.
 * Here every sensor is a plain IIO device with sysfs raw reads, scale,
 * sampling_frequency and mount_matrix, which is what iio-sensor-proxy's
 * iio-poll-* drivers use:
 *
 *   ssp-accel     in_accel_{x,y,z}_raw, in_accel_scale, in_accel_mount_matrix
 *   ssp-gyro      in_anglvel_{x,y,z}_raw, in_anglvel_scale, ..._mount_matrix
 *   ssp-magn      in_magn_{x,y,z}_raw, in_magn_scale, in_magn_mount_matrix
 *   ssp-light     in_illuminance_input (approximate lux), in_intensity_*_raw
 *   ssp-prox      in_proximity_raw (TMG3992 8-bit count; near >= ssp,prox-hi_thresh)
 *   ssp-pressure  in_pressure_raw/scale (kPa), in_temp_raw/scale
 *
 * A sensor is switched on in the hub on the first raw read and switched
 * off again after "idle_ms" without reads.  The hub already rotates
 * accel/gyro/mag into the Android device frame using ssp-acc-position /
 * ssp-mag-position, so the default mount matrix is identity; it can be
 * overridden with the DT properties accel-mount-matrix, gyro-mount-matrix
 * (falls back to accel-mount-matrix: same ICM20610 chip),
 * magn-mount-matrix / mag-mount-matrix, or the generic mount-matrix.
 */
#include "ssp.h"
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/property.h>
#include <linux/math64.h>

static unsigned int idle_ms = 3000;
module_param(idle_ms, uint, 0644);
MODULE_PARM_DESC(idle_ms, "switch a sensor off after this many ms without reads (0 = never)");

static unsigned int first_sample_ms = 600;
module_param(first_sample_ms, uint, 0644);
MODULE_PARM_DESC(first_sample_ms, "max wait for the first sample after enabling a sensor");

/* TMG3992 lux approximation (ams DN40 style), see ssp_light_lux() */
static unsigned int light_dgf = 310;
module_param(light_dgf, uint, 0644);
MODULE_PARM_DESC(light_dgf, "TMG3992 device/glass factor for the lux approximation");

struct ssp_iio_state {
	struct ssp_data *data;
	enum ssp_iio_sensor id;
	unsigned int hub_type;
	struct iio_mount_matrix orientation;
	struct mutex lock;
	struct delayed_work idle_work;
	bool active;
};

static const unsigned int ssp_iio_hub_type[SSP_IIO_MAX] = {
	[SSP_IIO_ACCEL]		= ACCELEROMETER_SENSOR,
	[SSP_IIO_GYRO]		= GYROSCOPE_SENSOR,
	[SSP_IIO_MAGN]		= GEOMAGNETIC_SENSOR,
	[SSP_IIO_LIGHT]		= LIGHT_SENSOR,
	[SSP_IIO_PROX]		= PROXIMITY_SENSOR,
	[SSP_IIO_PRESSURE]	= PRESSURE_SENSOR,
};

static const char *const ssp_iio_name[SSP_IIO_MAX] = {
	[SSP_IIO_ACCEL]		= "ssp-accel",
	[SSP_IIO_GYRO]		= "ssp-gyro",
	[SSP_IIO_MAGN]		= "ssp-magn",
	[SSP_IIO_LIGHT]		= "ssp-light",
	[SSP_IIO_PROX]		= "ssp-prox",
	[SSP_IIO_PRESSURE]	= "ssp-pressure",
};

static const char *const ssp_iio_matrix_prop[SSP_IIO_MAX] = {
	[SSP_IIO_ACCEL]		= "accel-mount-matrix",
	[SSP_IIO_GYRO]		= "gyro-mount-matrix",
	[SSP_IIO_MAGN]		= "magn-mount-matrix",
};

/* ------------------------------------------------------------------ */
/* data path (called from parse_dataframe via report_sensor_data[])   */
/* ------------------------------------------------------------------ */

static inline void ssp_new_sample(struct ssp_data *data, int type)
{
	data->data_seq[type]++;
	wake_up_all(&data->data_wq);
}

void report_meta_data(struct ssp_data *data, struct sensor_value *s)
{
	ssp_dbg_print("[SSP]: %s - what: %d, sensor: %d\n", __func__,
		s->meta_data.what, s->meta_data.sensor);
}

void report_acc_data(struct ssp_data *data, struct sensor_value *accdata)
{
	data->buf[ACCELEROMETER_SENSOR].x = accdata->x;
	data->buf[ACCELEROMETER_SENSOR].y = accdata->y;
	data->buf[ACCELEROMETER_SENSOR].z = accdata->z;
	data->buf[ACCELEROMETER_SENSOR].timestamp = accdata->timestamp;
	ssp_new_sample(data, ACCELEROMETER_SENSOR);
}

void report_gyro_data(struct ssp_data *data, struct sensor_value *gyrodata)
{
	data->buf[GYROSCOPE_SENSOR].x = gyrodata->x;
	data->buf[GYROSCOPE_SENSOR].y = gyrodata->y;
	data->buf[GYROSCOPE_SENSOR].z = gyrodata->z;
	data->buf[GYROSCOPE_SENSOR].timestamp = gyrodata->timestamp;
	ssp_new_sample(data, GYROSCOPE_SENSOR);
}

void report_geomagnetic_raw_data(struct ssp_data *data,
	struct sensor_value *magrawdata)
{
	data->buf[GEOMAGNETIC_RAW].x = magrawdata->x;
	data->buf[GEOMAGNETIC_RAW].y = magrawdata->y;
	data->buf[GEOMAGNETIC_RAW].z = magrawdata->z;
	ssp_new_sample(data, GEOMAGNETIC_RAW);
}

void report_mag_data(struct ssp_data *data, struct sensor_value *magdata)
{
	data->buf[GEOMAGNETIC_SENSOR].cal_x = magdata->cal_x;
	data->buf[GEOMAGNETIC_SENSOR].cal_y = magdata->cal_y;
	data->buf[GEOMAGNETIC_SENSOR].cal_z = magdata->cal_z;
	data->buf[GEOMAGNETIC_SENSOR].accuracy = magdata->accuracy;
	ssp_new_sample(data, GEOMAGNETIC_SENSOR);
}

void report_mag_uncaldata(struct ssp_data *data, struct sensor_value *magdata)
{
	data->buf[GEOMAGNETIC_UNCALIB_SENSOR] = *magdata;
	ssp_new_sample(data, GEOMAGNETIC_UNCALIB_SENSOR);
}

void report_uncalib_gyro_data(struct ssp_data *data, struct sensor_value *gyrodata)
{
	data->buf[GYRO_UNCALIB_SENSOR] = *gyrodata;
	ssp_new_sample(data, GYRO_UNCALIB_SENSOR);
}

void report_sig_motion_data(struct ssp_data *data,
	struct sensor_value *sig_motion_data)
{
	data->buf[SIG_MOTION_SENSOR].sig_motion = sig_motion_data->sig_motion;
	ssp_new_sample(data, SIG_MOTION_SENSOR);
}

void report_rot_data(struct ssp_data *data, struct sensor_value *rotdata)
{
	data->buf[ROTATION_VECTOR] = *rotdata;
	ssp_new_sample(data, ROTATION_VECTOR);
}

void report_game_rot_data(struct ssp_data *data, struct sensor_value *grvec_data)
{
	data->buf[GAME_ROTATION_VECTOR] = *grvec_data;
	ssp_new_sample(data, GAME_ROTATION_VECTOR);
}

void report_gesture_data(struct ssp_data *data, struct sensor_value *gesdata)
{
	memcpy(data->buf[GESTURE_SENSOR].data, gesdata->data, 20);
	ssp_new_sample(data, GESTURE_SENSOR);
}

void report_pressure_data(struct ssp_data *data, struct sensor_value *predata)
{
	data->buf[PRESSURE_SENSOR].pressure[0] =
		predata->pressure[0] - data->iPressureCal;
	data->buf[PRESSURE_SENSOR].pressure[1] = predata->pressure[1];
	ssp_new_sample(data, PRESSURE_SENSOR);
}

void report_light_data(struct ssp_data *data, struct sensor_value *lightdata)
{
	data->buf[LIGHT_SENSOR].r = lightdata->r;
	data->buf[LIGHT_SENSOR].g = lightdata->g;
	data->buf[LIGHT_SENSOR].b = lightdata->b;
	data->buf[LIGHT_SENSOR].w = lightdata->w;
	data->buf[LIGHT_SENSOR].a_time = lightdata->a_time;
	data->buf[LIGHT_SENSOR].a_gain = (0x03) & (lightdata->a_gain);
	ssp_new_sample(data, LIGHT_SENSOR);
}

void report_prox_data(struct ssp_data *data, struct sensor_value *proxdata)
{
	ssp_dbg_print("[SSP] Proximity Sensor Detect : %u, raw : %u\n",
		proxdata->prox[0], proxdata->prox[1]);

	data->buf[PROXIMITY_SENSOR].prox[0] = proxdata->prox[0];
	data->buf[PROXIMITY_SENSOR].prox[1] = proxdata->prox[1];
	ssp_new_sample(data, PROXIMITY_SENSOR);

	pm_wakeup_event(data->dev, 3000);
}

void report_prox_raw_data(struct ssp_data *data,
	struct sensor_value *proxrawdata)
{
	data->buf[PROXIMITY_RAW].prox[0] = proxrawdata->prox[0];
	ssp_new_sample(data, PROXIMITY_RAW);
}

void report_step_det_data(struct ssp_data *data,
		struct sensor_value *stepdet_data)
{
	data->buf[STEP_DETECTOR].step_det = stepdet_data->step_det;
	ssp_new_sample(data, STEP_DETECTOR);
}

void report_step_cnt_data(struct ssp_data *data,
	struct sensor_value *sig_motion_data)
{
	data->buf[STEP_COUNTER].step_diff = sig_motion_data->step_diff;
	data->step_count_total += data->buf[STEP_COUNTER].step_diff;
	ssp_new_sample(data, STEP_COUNTER);
}

void report_temp_humidity_data(struct ssp_data *data,
	struct sensor_value *temp_humi_data)
{
	data->buf[TEMPERATURE_HUMIDITY_SENSOR].x = temp_humi_data->x;
	data->buf[TEMPERATURE_HUMIDITY_SENSOR].y = temp_humi_data->y;
	data->buf[TEMPERATURE_HUMIDITY_SENSOR].z = temp_humi_data->z;
	ssp_new_sample(data, TEMPERATURE_HUMIDITY_SENSOR);
}

/* downstream ssp_misc.c forwarded this to the camera AF driver */
void report_shake_cam_data(struct ssp_data *data,
	struct sensor_value *shake_cam_data)
{
	data->buf[SHAKE_CAM].x = shake_cam_data->x;
	ssp_new_sample(data, SHAKE_CAM);
}

/* ------------------------------------------------------------------ */
/* on-demand enable                                                    */
/* ------------------------------------------------------------------ */

static void ssp_iio_idle_work(struct work_struct *work)
{
	struct ssp_iio_state *st = container_of(to_delayed_work(work),
						struct ssp_iio_state, idle_work);

	mutex_lock(&st->lock);
	if (st->active) {
		ssp_set_sensor_enable(st->data, st->hub_type, false);
		st->active = false;
	}
	mutex_unlock(&st->lock);
}

static int ssp_iio_get_sample(struct ssp_iio_state *st)
{
	struct ssp_data *data = st->data;
	unsigned int seq;
	int ret = 0;

	if (!data->bMcuReady)
		return -EAGAIN;	/* LHD has not sent ESW:READY yet */

	mutex_lock(&st->lock);
	if (!st->active) {
		seq = READ_ONCE(data->data_seq[st->hub_type]);
		ret = ssp_set_sensor_enable(data, st->hub_type, true);
		if (!ret) {
			st->active = true;
			/* wait for a fresh value instead of returning stale data */
			wait_event_interruptible_timeout(data->data_wq,
				READ_ONCE(data->data_seq[st->hub_type]) != seq,
				msecs_to_jiffies(first_sample_ms));
		}
	}
	if (st->active && idle_ms)
		mod_delayed_work(system_wq, &st->idle_work,
				 msecs_to_jiffies(idle_ms));
	mutex_unlock(&st->lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* IIO glue                                                            */
/* ------------------------------------------------------------------ */

static const struct iio_mount_matrix *
ssp_iio_get_mount_matrix(const struct iio_dev *indio_dev,
			 const struct iio_chan_spec *chan)
{
	struct ssp_iio_state *st = iio_priv(indio_dev);

	return &st->orientation;
}

static const struct iio_chan_spec_ext_info ssp_iio_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_TYPE, ssp_iio_get_mount_matrix),
	{ }
};

#define SSP_AXIS_CHAN(_type, _mod, _idx) {				\
	.type = (_type),						\
	.modified = 1,							\
	.channel2 = (_mod),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.ext_info = ssp_iio_ext_info,					\
	.scan_index = (_idx),						\
}

static const struct iio_chan_spec ssp_accel_channels[] = {
	SSP_AXIS_CHAN(IIO_ACCEL, IIO_MOD_X, 0),
	SSP_AXIS_CHAN(IIO_ACCEL, IIO_MOD_Y, 1),
	SSP_AXIS_CHAN(IIO_ACCEL, IIO_MOD_Z, 2),
};

static const struct iio_chan_spec ssp_gyro_channels[] = {
	SSP_AXIS_CHAN(IIO_ANGL_VEL, IIO_MOD_X, 0),
	SSP_AXIS_CHAN(IIO_ANGL_VEL, IIO_MOD_Y, 1),
	SSP_AXIS_CHAN(IIO_ANGL_VEL, IIO_MOD_Z, 2),
};

static const struct iio_chan_spec ssp_magn_channels[] = {
	SSP_AXIS_CHAN(IIO_MAGN, IIO_MOD_X, 0),
	SSP_AXIS_CHAN(IIO_MAGN, IIO_MOD_Y, 1),
	SSP_AXIS_CHAN(IIO_MAGN, IIO_MOD_Z, 2),
};

#define SSP_INTENSITY_CHAN(_mod, _idx) {				\
	.type = IIO_INTENSITY,						\
	.modified = 1,							\
	.channel2 = (_mod),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.scan_index = (_idx),						\
}

static const struct iio_chan_spec ssp_light_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = 0,
	},
	SSP_INTENSITY_CHAN(IIO_MOD_LIGHT_RED, 1),
	SSP_INTENSITY_CHAN(IIO_MOD_LIGHT_GREEN, 2),
	SSP_INTENSITY_CHAN(IIO_MOD_LIGHT_BLUE, 3),
	SSP_INTENSITY_CHAN(IIO_MOD_LIGHT_CLEAR, 4),
};

static const struct iio_chan_spec ssp_prox_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = 0,
	},
};

static const struct iio_chan_spec ssp_pressure_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = 0,
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = 1,
	},
};

/*
 * Approximate lux from the TMG3992 RGBC counts.  The Samsung HAL did this
 * in userspace with undisclosed coefficients; this uses the generic ams
 * formula: IR = (R+G+B-C)/2, lux = (G-IR) * DGF / (t_int[ms] * gain),
 * t_int = (256 - ATIME) * 2.78 ms, gain = 1/4/16/64.
 */
static int ssp_light_lux(struct ssp_data *data, int *val, int *val2)
{
	static const int again[4] = { 1, 4, 16, 64 };
	const struct sensor_value *v = &data->buf[LIGHT_SENSOR];
	int r = v->r, g = v->g, b = v->b, c = v->w;
	int ir = (r + g + b - c) / 2;
	int g1, atime = 256 - v->a_time;

	if (ir < 0)
		ir = 0;
	g1 = g - ir;
	if (g1 < 0)
		g1 = 0;
	if (atime <= 0)
		atime = 1;

	/* val/val2 = g1 * DGF / (atime * 2.78 * gain) */
	*val = g1 * (int)light_dgf;
	*val2 = (atime * 278 * again[v->a_gain & 3]) / 100;
	if (*val2 <= 0)
		*val2 = 1;
	return IIO_VAL_FRACTIONAL;
}

static int ssp_iio_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ssp_iio_state *st = iio_priv(indio_dev);
	struct ssp_data *data = st->data;
	const struct sensor_value *v = &data->buf[st->hub_type];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		ret = ssp_iio_get_sample(st);
		if (ret)
			return ret;

		switch (chan->type) {
		case IIO_ACCEL:
		case IIO_ANGL_VEL:
			*val = chan->channel2 == IIO_MOD_X ? v->x :
			       chan->channel2 == IIO_MOD_Y ? v->y : v->z;
			return IIO_VAL_INT;
		case IIO_MAGN:
			*val = chan->channel2 == IIO_MOD_X ? v->cal_x :
			       chan->channel2 == IIO_MOD_Y ? v->cal_y : v->cal_z;
			return IIO_VAL_INT;
		case IIO_LIGHT:
			return ssp_light_lux(data, val, val2);
		case IIO_INTENSITY:
			switch (chan->channel2) {
			case IIO_MOD_LIGHT_RED:
				*val = v->r;
				break;
			case IIO_MOD_LIGHT_GREEN:
				*val = v->g;
				break;
			case IIO_MOD_LIGHT_BLUE:
				*val = v->b;
				break;
			default:
				*val = v->w;
				break;
			}
			return IIO_VAL_INT;
		case IIO_PROXIMITY:
			*val = v->prox[1];
			return IIO_VAL_INT;
		case IIO_PRESSURE:
			*val = v->pressure[0];
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = v->pressure[1];
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			/* 4096 LSB/g (Samsung HAL: GRAVITY_EARTH / 4096) */
			*val = 0;
			*val2 = 2394202;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			/* +-2000 dps, 16.4 LSB/(deg/s) -> rad/s */
			*val = 0;
			*val2 = 1064225;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_MAGN:
			/* YAS532 via SSP: 0.06 uT/LSB = 0.0006 Gauss */
			*val = 0;
			*val2 = 600;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_PRESSURE:
			/* hub reports Pa (HAL divided by 100 for hPa) -> kPa */
			*val = 0;
			*val2 = 1000;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_TEMP:
			/* assumed 1/100 degC -> milli degC */
			*val = 10;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SAMP_FREQ: {
		s64 ns = data->adDelayBuf[st->hub_type];

		if (ns <= 0)
			return -EINVAL;
		*val = 1000000000;
		*val2 = (int)min_t(s64, ns, INT_MAX);
		return IIO_VAL_FRACTIONAL;
	}
	default:
		return -EINVAL;
	}
}

static int ssp_iio_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ssp_iio_state *st = iio_priv(indio_dev);
	u64 uhz;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	uhz = (u64)val * 1000000 + val2;
	if (!uhz || uhz > 1000ULL * 1000000)	/* 1 mHz .. 1 kHz */
		return -EINVAL;

	ssp_set_sensor_delay(st->data, st->hub_type,
			     div64_u64(1000000000ULL * 1000000ULL, uhz));
	return 0;
}

static const struct iio_info ssp_iio_info = {
	.read_raw = ssp_iio_read_raw,
	.write_raw = ssp_iio_write_raw,
};

static void ssp_iio_read_matrix(struct ssp_iio_state *st)
{
	struct device *dev = st->data->dev;
	const char *prop = ssp_iio_matrix_prop[st->id];
	static const char *const identity[9] = {
		"1", "0", "0", "0", "1", "0", "0", "0", "1"
	};
	int i;

	for (i = 0; i < 9; i++)
		st->orientation.rotation[i] = identity[i];

	if (prop && device_property_read_string_array(dev, prop,
			st->orientation.rotation, 9) == 9)
		return;
	/* treltexx-sensorhub.dtsi spells it "mag-mount-matrix" */
	if (st->id == SSP_IIO_GYRO &&
	    device_property_read_string_array(dev, "accel-mount-matrix",
			st->orientation.rotation, 9) == 9)
		return;
	if (st->id == SSP_IIO_MAGN &&
	    device_property_read_string_array(dev, "mag-mount-matrix",
			st->orientation.rotation, 9) == 9)
		return;
	if (iio_read_mount_matrix(dev, &st->orientation))
		for (i = 0; i < 9; i++)
			st->orientation.rotation[i] = identity[i];
}

int ssp_iio_init(struct ssp_data *data)
{
	static const struct {
		const struct iio_chan_spec *ch;
		int n;
	} chans[SSP_IIO_MAX] = {
		[SSP_IIO_ACCEL]    = { ssp_accel_channels, ARRAY_SIZE(ssp_accel_channels) },
		[SSP_IIO_GYRO]     = { ssp_gyro_channels, ARRAY_SIZE(ssp_gyro_channels) },
		[SSP_IIO_MAGN]     = { ssp_magn_channels, ARRAY_SIZE(ssp_magn_channels) },
		[SSP_IIO_LIGHT]    = { ssp_light_channels, ARRAY_SIZE(ssp_light_channels) },
		[SSP_IIO_PROX]     = { ssp_prox_channels, ARRAY_SIZE(ssp_prox_channels) },
		[SSP_IIO_PRESSURE] = { ssp_pressure_channels, ARRAY_SIZE(ssp_pressure_channels) },
	};
	int i, ret;

	for (i = 0; i < SSP_IIO_MAX; i++) {
		struct iio_dev *indio_dev;
		struct ssp_iio_state *st;

		indio_dev = iio_device_alloc(data->dev, sizeof(*st));
		if (!indio_dev) {
			ret = -ENOMEM;
			goto err;
		}
		st = iio_priv(indio_dev);
		st->data = data;
		st->id = i;
		st->hub_type = ssp_iio_hub_type[i];
		mutex_init(&st->lock);
		INIT_DELAYED_WORK(&st->idle_work, ssp_iio_idle_work);
		ssp_iio_read_matrix(st);

		indio_dev->name = ssp_iio_name[i];
		indio_dev->info = &ssp_iio_info;
		indio_dev->modes = INDIO_DIRECT_MODE;
		indio_dev->channels = chans[i].ch;
		indio_dev->num_channels = chans[i].n;

		ret = iio_device_register(indio_dev);
		if (ret) {
			iio_device_free(indio_dev);
			goto err;
		}
		data->indio[i] = indio_dev;
	}
	return 0;

err:
	ssp_iio_remove(data);
	return ret;
}

void ssp_iio_remove(struct ssp_data *data)
{
	int i;

	for (i = SSP_IIO_MAX - 1; i >= 0; i--) {
		struct iio_dev *indio_dev = data->indio[i];
		struct ssp_iio_state *st;

		if (!indio_dev)
			continue;
		st = iio_priv(indio_dev);
		iio_device_unregister(indio_dev);
		cancel_delayed_work_sync(&st->idle_work);
		mutex_destroy(&st->lock);
		iio_device_free(indio_dev);
		data->indio[i] = NULL;
	}
}
