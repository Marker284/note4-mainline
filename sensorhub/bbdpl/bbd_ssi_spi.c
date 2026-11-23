/*
 * Copyright 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *  
 * A copy of the GPL is available at 
 * http://www.broadcom.com/licenses/GPLv2.php, or by writing to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Interface from BBD to the SSI SPI:
 * (Broadcom Bridge Driver to the Serial Streaming Interface -SPI)
 * Placeholder until actual code is ready.
 *
 * tabstop = 8
 */

#include "bbd_internal.h"
#include "transport/bbd_engine.h"
#include "transport/bbd_bridge_c.h"
#include "bcm_gps_spi.h"

/*
 * Serial link between the BbdBridge and the 477x.
 *
 * Downstream: bridge -> bbd_tty_send() -> kernel filp on /dev/ttyBCM0 ->
 * fake uart in bcm_gps_spi.c -> SPI, and a kernel thread polling the tty for
 * the reverse direction.  Port: bridge -> bcm4773_tx_push() (fifo drained by
 * the SPI worker) and SPI worker -> rx fifo -> bbd_ssi_spi_on_rx() ->
 * BbdBridge_SetData().  /dev/ttyBCM0 is still provided by bcm_gps_spi.c for
 * raw userspace access.
 */

static atomic_t bbd_ssi_spi_run = ATOMIC_INIT(0);

int bbd_ssi_spi_open(void)
{
	if (!gpbbd_dev)
		return -ENODEV;
	if (atomic_xchg(&bbd_ssi_spi_run, 1))
		return 0;
	/* downstream bbd_tty_open() also opened the engine ("needed for JTAG
	 * download of ESW without LH daemon") */
	BbdEngine_Open(&gpbbd_dev->bbd_engine);
	return 0;
}

void bbd_ssi_spi_close(void)
{
	if (!atomic_xchg(&bbd_ssi_spi_run, 0))
		return;
	if (gpbbd_dev)
		BbdEngine_Close(&gpbbd_dev->bbd_engine);
}

int bbd_ssi_spi_send(unsigned char *buf, int count)
{
	bbd_log_hex(__func__, buf, count);
	return bcm4773_tx_push(buf, count, true);
}

void bbd_ssi_spi_on_rx(const unsigned char *buf, size_t len)
{
	struct BbdEngine *pEng;

	if (!gpbbd_dev)
		return;
	pEng = bbd_engine();
	if (!pEng->open) {
		dprint("engine closed, %zu rx bytes ignored\n", len);
		return;
	}
	bbd_log_hex(__func__, buf, len);
	while (len) {
		unsigned short n = min_t(size_t, len, 0x8000);

		BbdBridge_SetData(&pEng->bridge, buf, n);
		buf += n;
		len -= n;
	}
}

//-----------------------------------------------------------------------------
//
//      bbd_ssi_spi_debug
//
//  Use debug mode to push download data directly into 477x memory,
//  bypassing all TL, RPCs, CRC, etc.
//
//-----------------------------------------------------------------------------

struct bbd_ssi_spi_debug_device *bbd_ssi_spi_debug_ptr(void)
{
        if (!gpbbd_dev)
                return 0;
        return (struct bbd_ssi_spi_debug_device *) gpbbd_dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG];
}

static __maybe_unused struct bbd_base *bbd_ssi_spi_debug_ptr_base(void)
{
        struct bbd_ssi_spi_debug_device *ps = bbd_ssi_spi_debug_ptr();
        if (!ps)
                return 0;
        return &ps->base;
}

static int bbd_ssi_spi_debug_open(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "BBD:%s()\n", __func__);

	if (!gpbbd_dev)
		return -EFAULT;

	filp->private_data = gpbbd_dev;

	return 0;
}

static int bbd_ssi_spi_debug_release(struct inode *inode, struct file *filp)
{
	return 0;
}

//-----------------------------------------------------------------------------
//
//      bbd_ssi_spi_debug_init_vars
//
//-----------------------------------------------------------------------------

void bbd_ssi_spi_debug_init_vars(struct bbd_device* dev, int* result)
{
        struct bbd_ssi_spi_debug_device* p = 0;

        if (*result)
                return;

        p = bbd_alloc(sizeof(struct bbd_ssi_spi_debug_device));
        if (!p) {
                *result = -ENOMEM;
                return;
        }

        bbd_base_init_vars(dev, &p->base, result);
        if (*result) {
                bbd_free(p);
                return;
        }
        p->numXfers = 0;
        p->numBytes = 0;
        dev->bbd_ptr[BBD_MINOR_SSI_SPI_DEBUG] = p;
        p->base.name = "ssi_spi_debug";
}

/* userspace ABI (LHD, 32-bit ARM): 12 bytes { u32 addr; u8 *buf; s32 size } */
struct bbd_download_direct
{
        unsigned long  addr;
        unsigned char __user *buf;  // CAUTION: user address
        int            size;
};

static
ssize_t bbd_ssi_spi_debug_data(const struct bbd_download_direct* d,
                               unsigned char*                    spi_buf,
                               size_t*                           payload_len);
static
ssize_t bbd_ssi_spi_debug_addr(const struct bbd_download_direct* d,
                               unsigned char*                    spi_buf);


//-----------------------------------------------------------------------------
//
//      bbd_ssi_spi_debug_read
//
//-----------------------------------------------------------------------------

static ssize_t bbd_ssi_spi_debug_read( struct file *filp,
			char __user *buf,
			size_t size, loff_t *ppos)
{
        int status = 1;
        struct bbd_download_direct d;
	int            rx_len,tx_len;
        unsigned char tx_dbg_read[256 + 3] = {0x80, 0xD1, 0x00};  //SSI_MODE_DEBUG | SSI_MODE_HALF_DUPLEX | SSI_WRITE_TRANS, 0x68,RW=1, DMA Start Addr offset
        unsigned char rx_dbg_read[256 + 3] = {0x00, 0x00, 0x00};
        unsigned char tx_dbg_rfifo_depth[4] = {0xA0, 0xD1, (0x21<<1), 0x00};  //SSI_MODE_DEBUG | SSI_MODE_HALF_DUPLEX | SSI_READ_TRANS, 0x68,RW=1, RFIFO Depth offset
        unsigned char rx_dbg_rfifo_depth[4] = {0x00, 0x00, 0x00, 0x00};

        dprint("%s(%zu)\n", __func__, size);
        if (size != sizeof(d))
                return -EINVAL;

        if (copy_from_user(&d, buf, size))
                return -EFAULT;
	if (d.size < 0)
		return -EINVAL;

	rx_len = d.size;
	tx_len = 0;

        tx_dbg_read[3] = (unsigned char)(d.addr & 0xff);
        tx_dbg_read[4] = (unsigned char)((d.addr >> 8)  & 0xff);
        tx_dbg_read[5] = (unsigned char)((d.addr >> 16) & 0xff);
        tx_dbg_read[6] = (unsigned char)((d.addr >> 24)  & 0xff);

        tx_dbg_read[7] = (unsigned char)(d.size & 0xff);
        tx_dbg_read[8] = (unsigned char)((d.size >> 8) & 0xff);

        tx_dbg_read[9] = 1;  // Bit0: 0:disable/done, 1:enable

        bcm477x_debug_transfer(tx_dbg_read,rx_dbg_read,10,0);

        while ( rx_len && status ) {
	      //Read target RFIFO Depth to determine how much we can read from it
	      bcm477x_debug_transfer(tx_dbg_rfifo_depth,rx_dbg_rfifo_depth,4,0);

	      // rx_dbg_rfifo_depth[1] = 0xD1 (0x68,RW=1), rx_dbg_rfifo_depth[2] = RFIFO Depth
	      status = (int)rx_dbg_rfifo_depth[2];
	      if ( status ) {
	         memset(tx_dbg_read, 0x00, sizeof(tx_dbg_read));
	         memset(rx_dbg_read, 0x00, sizeof(rx_dbg_read));

	         tx_dbg_read[0] = 0xA0;
	         tx_dbg_read[1] = 0xD1;
	         tx_dbg_read[3] = (0x22 << 1);  // RFIFO Read Data

	         bcm477x_debug_transfer(tx_dbg_read,rx_dbg_read,status + 3,0);

	         if  ( rx_len < status ) status = rx_len;  // To be safe only
		 /*
		  * Port: downstream copied into "buf + tx_len", i.e. over the
		  * request structure itself, and its transfer helper never
		  * returned rx data (so this loop never ran).  Copy into the
		  * caller-supplied data buffer instead.
		  */
                 if (copy_to_user(d.buf + tx_len, rx_dbg_read + 2, status))
		       return -EFAULT;

		 rx_len -= status;
		 tx_len += status;
	      }
        }

        return d.size - rx_len;
}

//-----------------------------------------------------------------------------
//
//      bbd_ssi_spi_debug_write
//
//-----------------------------------------------------------------------------

static ssize_t bbd_ssi_spi_debug_write(struct file         *filp,
                                const char __user   *buf,
                                size_t size, loff_t *ppos)
{
        struct bbd_download_direct d;

        dprint("%s(%zu)\n", __func__, size);
        if (size != sizeof(d))
                return -EINVAL;

        if (copy_from_user(&d, buf, size))
                return -EFAULT;

        do
        {
            size_t         spi_buf_len = 0;
            ssize_t        xfer_len;
            unsigned char* spi_buf     = bcm477x_debug_buffer(&spi_buf_len);
            size_t         payload_len = 0;
            int            written     = 0;
	
            if (!spi_buf || spi_buf_len < sizeof(d))
                    return -EINVAL;

//hoi test
		{
			int tx_size = d.size+3;
			d.size += 16 - (tx_size % 16);
		}

            // Send the addr+len+DMA start command:
            xfer_len = bbd_ssi_spi_debug_addr(&d, spi_buf);
            dprint("%zd\n", xfer_len);
            if (xfer_len < 1)
                    return xfer_len;

            written = bcm477x_debug_write(spi_buf, xfer_len, 0);
            dprint("%zd %d\n", xfer_len, written);
            if (written != xfer_len)
                    return -EINVAL;

            // Send the DATA:
            xfer_len = bbd_ssi_spi_debug_data(&d, spi_buf, &payload_len);
            dprint("%zd\n", xfer_len);
            if (xfer_len < 0)
                    return xfer_len;

            written = bcm477x_debug_write(spi_buf, xfer_len, 0);
            dprint("%zd %d\n", xfer_len, written);
            if (written != xfer_len)
                    return -EINVAL;

            d.size -= payload_len;
            d.addr += payload_len;
            d.buf  += payload_len;
        } while (d.size > 0);

        return size;
}

//-----------------------------------------------------------------------------
//
//      bbd_ssi_spi_debug_addr
//      bbd_ssi_spi_debug_data
//
//      Fill the buffer with SSI-debug data.
//      Do two transfers inside one transaction:
//      a)  DebugMode(addr+len)
//      b)  DebugMode(data)
//
//-----------------------------------------------------------------------------

// True overhead for SSI:  (no b_data[1])
#define SSI_DEBUG_OVERHEAD    4

#define MAX_TX_DEBUG_BUF_SIZE (MAX_TX_RX_BUF_SIZE - SSI_DEBUG_OVERHEAD)

#define SSI_SPI_SLAVE_ADDR     0x68     // From Romi's spec.
#define SSI_SPI_SLAVE_WR       0x01
#define SSI_SPI_SLAVE_RD       0x00

#define SSI_SPI_SLAVE          ((SSI_SPI_SLAVE_ADDR << 1) | SSI_SPI_SLAVE_WR)

#define SSI_SPI_DMA_START_ADDR 0x00
#define SSI_I2C_DMA_DATA_ADDR  0x12             // From Romi's I2C DEBUG spec.
#define SSI_SPI_DMA_DATA_ADDR (SSI_I2C_DMA_DATA_ADDR << 1)

#define DMA_CONTROL_ENABLE     0x1
#define DMA_CONTROL_WRITE      0x2
#define DMA_CONTROL            (DMA_CONTROL_ENABLE | DMA_CONTROL_WRITE)

static ssize_t bbd_ssi_spi_debug_addr(const struct bbd_download_direct* d,
                               unsigned char*                    spi_buf)
{
    // size_t len = orig_len;
    struct Bcm477x_Debug_Xfer*       x = (struct Bcm477x_Debug_Xfer*) spi_buf;
    struct bbd_ssi_spi_debug_device* p = bbd_ssi_spi_debug_ptr();

    union long_union
    {
        unsigned char uc[sizeof(unsigned long)];
        unsigned long ul;
    } swap_addr;

    union short_union
    {
        unsigned char  uc[sizeof(unsigned short)];
        unsigned short us;
    } swap_len;

    int len = d->size;
    if (len > MAX_TX_DEBUG_BUF_SIZE)
        len = MAX_TX_DEBUG_BUF_SIZE;

    x->a_mode     = (SSI_MODE_DEBUG | SSI_MODE_HALF_DUPLEX | SSI_WRITE_TRANS);
    x->a_cmd      = SSI_SPI_SLAVE;
    x->a_spi_addr = SSI_SPI_DMA_START_ADDR;

    swap_addr.ul = d->addr;
    x->a_addr[0] = swap_addr.uc[0];
    x->a_addr[1] = swap_addr.uc[1];
    x->a_addr[2] = swap_addr.uc[2];
    x->a_addr[3] = swap_addr.uc[3];

    swap_len.us = len;
    x->a_len[0] = swap_len.uc[0];
    x->a_len[1] = swap_len.uc[1];
    x->a_dma    = DMA_CONTROL;

    len = 10;
    if (p)
    {
        memcpy(&p->trace, x, len);
        ++p->numXfers;
    }

    if (bbd_debug())
    {
        const unsigned char* p = spi_buf;
        printk("bbd_ssi_spi_debug_addr(0x%lx, 0x%lx[%d] = \n",
                 (unsigned long)d->addr,  (unsigned long)d->buf,  d->size);
        printk("  a { %02X %02X %02X   %02X.%02X.%02X.%02X  %02X.%02X  %02X }\n",
                      p[0],p[1],p[2],  p[3],p[4],p[5],p[6], p[7],p[8], p[9]     );
        // Expect:    80,  D1,  00,    address(byte-swap)   len(swap), 03
    }
    return len;
}

static ssize_t bbd_ssi_spi_debug_data(const struct bbd_download_direct* d,
                               unsigned char*                    spi_buf,
                               size_t*                           payload_len)
{
    struct Bcm477x_Debug_Xfer*       x = (struct Bcm477x_Debug_Xfer*) spi_buf;
    struct bbd_ssi_spi_debug_device* p = bbd_ssi_spi_debug_ptr();

    int len = d->size;
    if (len > MAX_TX_DEBUG_BUF_SIZE)
        len = MAX_TX_DEBUG_BUF_SIZE;

    if (!payload_len)
        return -EINVAL;

    x->a_mode     = (SSI_MODE_DEBUG | SSI_MODE_HALF_DUPLEX | SSI_WRITE_TRANS);
    x->a_cmd      = SSI_SPI_SLAVE;
    x->a_spi_addr = SSI_SPI_DMA_DATA_ADDR;

    if (copy_from_user(x->a_addr, d->buf, len))
        return -EFAULT;

    if (p)
    {
        memcpy(&p->trace.b_mode, x, 4);
        p->numBytes += len;
    }

    if (bbd_debug())
    {
        const unsigned char* p = spi_buf;
        printk("bbd_ssi_spi_debug_data(0x%lx, 0x%lx[%d] = \n",
                 (unsigned long)d->addr,  (unsigned long)d->buf,  d->size);
        printk("  b { %02X %02X %02X   %02X,%02X,%02X,%02X... }[%d]\n",
                      p[0],p[1],p[2],  p[3],p[4],p[5],p[6],    len);
        // Expect:    80,  D1,  24,    data...       
    }
    *payload_len = len;
    return len + 3;
}

const struct file_operations bbd_ssi_spi_debug_fops = {
	.owner		= THIS_MODULE,
	.open		= bbd_ssi_spi_debug_open,
	.release	= bbd_ssi_spi_debug_release,
	.read		= bbd_ssi_spi_debug_read,
	.write		= bbd_ssi_spi_debug_write,
	.llseek		= noop_llseek,
};
