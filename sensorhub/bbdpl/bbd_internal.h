/*
 * Copyright 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * The BBD (Broadcom Bridge Driver)
 *
 * Ported to Linux 7.3 (out-of-tree): poll returns __poll_t, the tty
 * plumbing (bbd_tty.c, filp_open("/dev/ttyBCM0") from the kernel) was
 * replaced by a direct link to the SSI-SPI transport in bcm_gps_spi.c.
 */

#ifndef BBD_INTERNAL_H__        /* { */
#define BBD_INTERNAL_H__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/list.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/sysfs.h>
#include <linux/cdev.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/poll.h>

#include "bbd.h"
#include "bbd_ifc.h"
#include "bbd_debug.h"
#include "transport/bbd_engine.h"

#define BBD_DEVICE_MAJOR	239

#define BBD_PACKET_TIMEOUT	1000

enum {
    BBD_MINOR_SENSOR    = 0,
    BBD_MINOR_CONTROL   = 1,
    BBD_MINOR_PACKET    = 2,
    BBD_MINOR_RELIABLE  = 3,
    BBD_MINOR_PATCH     = 4,
    BBD_MINOR_SSI_SPI_DEBUG = 5,
    BBD_DEVICE_INDEX
};

struct bbd_dev;

struct bbd_buffer {
	struct mutex lock;		/* lock */
	wait_queue_head_t poll_inq;	/* wait queue for poll */
	wait_queue_head_t comp_inq;	/* wait queue for completion */
	unsigned int count;		/* total size count */
	unsigned int tout_cnt;		/* timeout count */

	unsigned char *pbuff;		/* buff pointer for tx */
	int buff_size;			/* buff size */
};

#define BBD_BUFFER_SIZE 1024

struct bbd_qitem {
        struct bbd_qitem *next;
        unsigned short    len;
        unsigned short    lenUsed;
        unsigned char     data[BBD_BUFFER_SIZE];
};

#define QITEM_OVERHEAD  (sizeof(struct bbd_qitem) - BBD_BUFFER_SIZE)


struct bbd_queue {
	struct mutex lock;		/* lock */
	wait_queue_head_t poll_inq;	/* wait queue for poll */
	wait_queue_head_t comp_inq;	/* wait queue for completion */
	unsigned int count;		/* total size count */
	unsigned int tout_cnt;		/* timeout count */

	struct bbd_qitem *qhead;	/* queue of data */
	struct bbd_qitem *qtail;	/* last item in q */
};

#define BBD_SERIAL_SPI  0               /* stacked ssi-spi driver for 4773 */
#define BBD_SERIAL_TTY  1               /* stacked tty driver for 477x */

#define BBD_NEW_HW_REV  8               /* connnected mcu_req, mcu_resp from HW rev 8 */

/*
 * bbd device structure
 */
struct bbd_device {
        struct BbdEngine    bbd_engine;
	struct cdev dev    [BBD_DEVICE_INDEX];  /* device for bbd */
	void       *bbd_ptr[BBD_DEVICE_INDEX];  /* individual structures */

	struct mutex      qlock;
	struct bbd_qitem *qfree;	    /* list of free qitems */
        int               qcount;

	struct mutex      cb_lock;	    /* protects sensor callbacks */

        bool        db;                     /* debug flag */
        bool        shmd;                   /* enable shmd-->bridge flow */
        int         sio_type;               /* 0 [default] SPI, 1 TTY */
        unsigned long jiffies_to_wake;
        int           tty_baud;

        unsigned int hw_rev;
	struct device *pdev;		    /* /sys/devices/platform/bbd */
};

extern struct bbd_device *gpbbd_dev;

/*
 * bbd common device structure
 */
struct bbd_base {
	struct bbd_queue  txq;              /* tx buffer LHD<--BBD */
	struct bbd_buffer rxb;              /* rx buffer LHD-->BBD */
        const char* name;
};

ssize_t      bbd_write  (int minor, const          char *pbuff, size_t size, bool bUser);
ssize_t      bbd_on_read(int minor, const unsigned char *pbuff, size_t size);
ssize_t      bbd_read   (int minor, char __user         *pbuff, size_t size, bool bUser);
__poll_t     bbd_poll   (int minor, struct file *filp, poll_table *wait);
int          bbd_count  (int minor);

/*
 * bbd sensor device structure
 */
struct bbd_sensor_device {
        struct bbd_base base;
	void           *priv_data;        /* private data pointer	*/
        bbd_callbacks  *callbacks;
};

struct bbd_control_device {
        struct bbd_base   base;
	bool              esw_ready;
};

struct bbd_packet_device {
        struct bbd_base base;
};

struct bbd_reliable_device {
        struct bbd_base base;
};

struct Bcm477x_Debug_Xfer
{
        unsigned char a_mode;         // 0x80
        unsigned char a_cmd;          // 0xD1
        unsigned char a_spi_addr;     // 0x00
        unsigned char a_addr[4];
        unsigned char a_len [2];
        unsigned char a_dma;          // 0x03
        unsigned char b_mode;         // 0x80
        unsigned char b_cmd;          // 0xD1
        unsigned char b_spi_addr;     // 0x24
        unsigned char b_data[1];      // get actual size from a_len[]
};

struct bbd_ssi_spi_debug_device {
        struct bbd_base base;
        unsigned long numXfers;
        unsigned long numBytes;
        struct Bcm477x_Debug_Xfer trace;
};

void bbd_base_init_vars(struct bbd_device *dev, struct bbd_base *p, int* result);
int  bbd_base_uninit   (                        struct bbd_base *p             );
int  bbd_base_reinit   (int minor);
int  bbd_queue_uninit  (struct bbd_qitem *q);
int  bbd_qitem_cache_uninit(void);
int  bbd_alloc_count(void);

void  bbd_free (void  *p   );
void* bbd_alloc(size_t size);
#define bbd_calloc bbd_alloc

struct BbdEngine *bbd_engine(void);

/* per-minor helpers (bbd_*.c) */
struct bbd_sensor_device  *bbd_sensor_ptr(void);
struct bbd_base           *bbd_sensor_ptr_base(void);
struct bbd_control_device *bbd_control_ptr(void);
struct bbd_ssi_spi_debug_device *bbd_ssi_spi_debug_ptr(void);

void bbd_sensor_init_vars  (struct bbd_device *dev, int* result);
void bbd_control_init_vars (struct bbd_device *dev, int* result);
void bbd_packet_init_vars  (struct bbd_device *dev, int* result);
void bbd_reliable_init_vars(struct bbd_device *dev, int* result);
void bbd_patch_init_vars   (struct bbd_device *dev, int* result);
void bbd_ssi_spi_debug_init_vars(struct bbd_device *dev, int* result);

int bbd_sensor_uninit(void);
int bbd_control_uninit(void);
int bbd_packet_uninit(void);
int bbd_reliable_uninit(void);
int bbd_patch_uninit(void);

extern const struct file_operations bbd_sensor_fops;
extern const struct file_operations bbd_control_fops;
extern const struct file_operations bbd_packet_fops;
extern const struct file_operations bbd_reliable_fops;
extern const struct file_operations bbd_patch_fops;
extern const struct file_operations bbd_ssi_spi_debug_fops;

ssize_t bbd_ESW_control(char *buf, ssize_t len);
ssize_t bbd_sensor_write_internal(const char *sensorData, size_t size);

int  bbd_sysfs_init  (struct kobject *kobj);
void bbd_sysfs_uninit(struct kobject *kobj);

void BbdEngine_Open (struct BbdEngine *p);
void BbdEngine_Close(struct BbdEngine *p);

/*
 * Serial link to the 477x (replaces bbd_tty.c / bbd_sio.c).
 * Implemented in bbd_ssi_spi.c on top of bcm_gps_spi.c.
 */
int  bbd_ssi_spi_open(void);
void bbd_ssi_spi_close(void);
int  bbd_ssi_spi_send(unsigned char *buf, int count);
/* called by bcm_gps_spi.c (process context) with bytes read from the chip */
void bbd_ssi_spi_on_rx(const unsigned char *buf, size_t len);

/* bcm_gps_spi.c */
int  bcm_gps_spi_init(void);
void bcm_gps_spi_exit(void);
int  bcm4773_tx_push(const unsigned char *buf, size_t len, bool may_wait);
void bcm4773_mcu_resp_float(bool do_float);
unsigned char *bcm477x_debug_buffer(size_t *len);
int  bcm477x_debug_write(const unsigned char *buf, size_t len, int flag);
int  bcm477x_debug_transfer(const unsigned char *tx, unsigned char *rx, int len, int flag);

/* bbd_patch.c */
extern char *bbd_patch_fw_name;

/* debug switches (exported for ssp_bbd.ko) */
extern bool ssp_dbg;
extern bool ssp_pkt_dbg;
extern bool ssi_dbg;

#endif /* BBD_INTERNAL_H__        } */
