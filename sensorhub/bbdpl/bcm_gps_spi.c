/*
 * Driver for the BCM4773 SPI/SSI interface (Galaxy Note 4 Exynos sensor hub)
 *
 * Linux 7.3 port of drivers/sensorhub/brcm/bbdpl/bcm_gps_spi.c (v1.26, KOM).
 *
 * What changed against downstream:
 *  - no serial_core "fake UART" any more.  The downstream data path was
 *    BbdBridge -> kernel filp_open("/dev/ttyBCM0") -> uart xmit buffer ->
 *    this driver; now the bridge talks to this driver directly through a
 *    tx kfifo (bcm4773_tx_push) and an rx kfifo + worker
 *    (bbd_ssi_spi_on_rx).  /dev/ttyBCM0 still exists (module param "tty")
 *    as a plain tty_driver for raw userspace access; while it is open,
 *    received bytes go to the tty instead of the bridge.
 *  - gpiod instead of of_get_named_gpio/s3c_gpio_*; pull-ups come from
 *    pinctrl.  DT names: host-req-gpios, mcu-req-gpios, mcu-resp-gpios,
 *    optional enable-gpios (GPS_PWR_EN); the downstream-style names
 *    ssp-irq-gpios / ssp-mcu-req-gpios / ssp-mcu-resp-gpios /
 *    gps-pwr-en-gpios are accepted too.  IRQ: first "interrupts" entry
 *    of the node, else gpiod_to_irq(HOST_REQ).
 *  - the IRQ depth hacks (irq_to_desc()->depth = ...) are gone; the
 *    irq_enabled flag alone keeps disable/enable balanced.
 *  - wake_lock -> pm_wakeup_event(); spi .suspend/.resume -> dev_pm_ops.
 *  - the SSP sensor driver is no longer probed via pssp_driver->probe(spi);
 *    a child platform device "bbd-ssp" (sharing the SPI node) is created
 *    and ssp_bbd.ko binds to it.
 *  - 32 bit SPI words for long half-duplex writes are off by default
 *    (module param bpw32): mainline spi-s3c64xx has no byte swap, so 32
 *    bit words would reorder bytes on the wire.
 *  - autobaud / identify were compiled out downstream and are dropped.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/kfifo.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>

#include "bcm_gps_spi.h"
#include "bbd_internal.h"

#define GPS_VERSION    "1.26-7.3"
#define PFX            "bcmgps: "

bool ssi_dbg;
EXPORT_SYMBOL_GPL(ssi_dbg);

static bool use_tty = true;
module_param_named(tty, use_tty, bool, 0444);
MODULE_PARM_DESC(tty, "create /dev/ttyBCM0 for raw userspace access");

static bool bpw32;
module_param(bpw32, bool, 0644);
MODULE_PARM_DESC(bpw32, "use 32 bit SPI words for long half-duplex writes (needs controller byte swap)");

#define BCM4773_XMIT_SIZE	4096		/* downstream UART_XMIT_SIZE */
#define BCM4773_TX_FIFO_SIZE	(32 * 1024)
#define BCM4773_RX_FIFO_SIZE	(64 * 1024)
#define SPI_DMA_BYTES_PER_WORD	4
#define WORD_BURST_SIZE		4

struct bcm4773_message {
	unsigned char	cmd_stat;
	unsigned char	data[BCM4773_XMIT_SIZE];
} __packed;

struct bcm4773_port {
	struct spi_device	*spi;
	struct gpio_desc	*host_req;
	struct gpio_desc	*mcu_req;
	struct gpio_desc	*mcu_resp;
	struct gpio_desc	*pwr_en;
	int			irq;

	atomic_t		irq_enabled;
	atomic_t		suspending;
	bool			running;

	struct workqueue_struct	*wq;		/* rxtx (ordered) */
	struct workqueue_struct	*rx_wq;		/* rx delivery (ordered) */
	struct work_struct	rxtx_work;
	struct work_struct	rx_work;

	struct kfifo		txf;
	spinlock_t		tx_lock;
	wait_queue_head_t	tx_wait;
	struct kfifo		rxf;
	spinlock_t		rx_lock;

	struct mutex		spi_lock;
	struct bcm4773_message	*tx_msg, *rx_msg;	/* stream mode */
	struct bcm4773_message	*dbg_tx, *dbg_rx;	/* SSI debug mode */

	/* statistics (downstream: uart_port icount) */
	unsigned long		cnt_rx, cnt_tx, cnt_brk, cnt_overrun, cnt_buf_overrun;

	struct tty_port		tty_port;
	struct device		*tty_dev;

	struct platform_device	*ssp_pdev;

	struct device		*gps_dev;
	struct device		*pwr_dev;
};

static struct bcm4773_port *g_bport;
static DEFINE_MUTEX(g_bport_lock);
static struct tty_driver *bcm4773_tty_driver;

/* ------------------------------------------------------------------------
 * MCU_REQ / MCU_RESP handshake ("DDS")
 * --------------------------------------------------------------------- */

static bool bcm4773_hello(struct bcm4773_port *bport)
{
	int count = 0, retries = 0;

	if (!bport->mcu_req || !bport->mcu_resp)
		return true;

	gpiod_set_value_cansleep(bport->mcu_req, 1);
	while (!gpiod_get_value_cansleep(bport->mcu_resp)) {
		if (count++ > 100) {
			gpiod_set_value_cansleep(bport->mcu_req, 0);
			pr_err_ratelimited(PFX "MCU_RESP not responding to MCU_REQ\n");
			return false;
		}

		usleep_range(1000, 1100);

		if (gpiod_get_value_cansleep(bport->mcu_resp))
			break;

		if (count % 20 == 0 && retries++ < 3) {
			gpiod_set_value_cansleep(bport->mcu_req, 0);
			usleep_range(1000, 1100);
			gpiod_set_value_cansleep(bport->mcu_req, 1);
			usleep_range(1000, 1100);
		}
	}
	return true;
}

static void bcm4773_bye(struct bcm4773_port *bport)
{
	if (bport->mcu_req)
		gpiod_set_value_cansleep(bport->mcu_req, 0);
}

/* Downstream toggled the MCU_RESP pull (always to pull-up); pinctrl does
 * that now. */
void bcm4773_mcu_resp_float(bool do_float)
{
}

/* ------------------------------------------------------------------------
 * SSI transfers
 * --------------------------------------------------------------------- */

static void pk_log(struct device *dev, char dir, unsigned char cmd_stat,
		   const unsigned char *data, int len)
{
	if (likely(!ssi_dbg))
		return;
	dev_info(dev, "%c 0x%02X %5d\n", dir, cmd_stat, len);
	print_hex_dump(KERN_INFO, "ssi: ", DUMP_PREFIX_OFFSET, 32, 1,
		       data, min(len, 256), false);
}

/* returns status (cmd_stat | data[0] << 8), 0xFF on error */
static bcm4773_stat_t bcm4773_transfer(struct bcm4773_port *bport,
				       unsigned char cmd_stat, int length)
{
	struct bcm4773_message *wr = bport->tx_msg;
	struct bcm4773_message *rd = bport->rx_msg;
	struct spi_transfer t = { };
	int status;

	if (length > (int)sizeof(wr->data) || length == 0)
		return 0xFF;

	t.tx_buf = wr;
	t.rx_buf = rd;
	t.bits_per_word = 8;

	wr->cmd_stat = cmd_stat;

	if ((cmd_stat & SSI_READ_TRANS) == 0) {
		if (cmd_stat & SSI_MODE_FULL_DUPLEX) {
			wr->data[0] = length;
			length += 1;	/* RX_PKT_LEN byte in full duplex */
		} else {
			t.rx_buf = NULL;
			rd->cmd_stat = 0;
			rd->data[0] = 0;
		}
	}

	pk_log(&bport->spi->dev, (cmd_stat & SSI_READ_TRANS) ? 'W' : 'w',
	       cmd_stat, wr->data, length);

	t.len = length + 1;	/* + cmd_stat */

	if (bpw32 && cmd_stat == (SSI_MODE_HALF_DUPLEX | SSI_WRITE_TRANS) &&
	    !(t.len % (SPI_DMA_BYTES_PER_WORD * WORD_BURST_SIZE)))
		t.bits_per_word = 32;

	mutex_lock(&bport->spi_lock);
	status = spi_sync_transfer(bport->spi, &t, 1);
	mutex_unlock(&bport->spi_lock);
	if (status)
		return 0xFF;

	pk_log(&bport->spi->dev, (cmd_stat & SSI_READ_TRANS) ? 'R' : 'r',
	       rd->cmd_stat, rd->data, length);

	return (bcm4773_stat_t)rd->cmd_stat | ((bcm4773_stat_t)rd->data[0] << 8);
}

static bcm4773_stat_t bcm4773_read(struct bcm4773_port *bport,
				   unsigned char cmd_stat, int length)
{
	memset(bport->tx_msg->data, 0, length);
	return bcm4773_transfer(bport, cmd_stat, length);
}

static inline bcm4773_stat_t bcm4773_read_status(struct bcm4773_port *bport)
{
	return bcm4773_read(bport, SSI_READ_TRANS | SSI_MODE_HALF_DUPLEX, 1);
}

static bcm4773_stat_t bcm4773_read_transmit(struct bcm4773_port *bport,
					    int *length)
{
	bcm4773_stat_t retval = bcm4773_read_status(bport);
	bcm4773_stat_t status = BCM4773_MSG_STAT(retval);

	*length = 0;
	if (status == 0) {
		*length = BCM4773_MSG_LEN(retval);
		if (*length != 0) {
			int len2;

			/* 255 + 2 header bytes would trigger the buggy DMA */
			if (*length == 255)
				*length = 254;
			retval = bcm4773_read(bport, SSI_READ_TRANS |
					      SSI_MODE_HALF_DUPLEX, *length + 1);
			len2 = BCM4773_MSG_LEN(retval);
			if (len2 < *length) {
				pr_err(PFX "error reading from the chipset! Read %d, expected %d\n",
				       len2, *length);
				*length = len2;
				bport->cnt_brk++;
			}
		}
	}
	return retval;
}

static bcm4773_stat_t bcm4773_write_receive(struct bcm4773_port *bport,
					    int n, int *length)
{
	bcm4773_stat_t retval = bcm4773_transfer(bport, SSI_WRITE_TRANS |
						 SSI_MODE_FULL_DUPLEX, n);
	int len2 = BCM4773_MSG_LEN(retval);

	*length = (n > len2) ? len2 : n;
	return retval;
}

static void bcm4773_reset_fifo(struct bcm4773_port *bport)
{
	int i, length;

	for (i = 0; i < 3; i++)
		bcm4773_read_transmit(bport, &length);
}

/* ------------------------------------------------------------------------
 * RX path
 * --------------------------------------------------------------------- */

static void bcm4773_rx_insert(struct bcm4773_port *bport,
			      const unsigned char *data, int length)
{
	struct tty_struct *tty;
	unsigned int n;

	if (length <= 0)
		return;

	bport->cnt_rx += length;

	tty = use_tty ? tty_port_tty_get(&bport->tty_port) : NULL;
	if (tty) {
		n = tty_insert_flip_string(&bport->tty_port, data, length);
		if (n < length)
			bport->cnt_buf_overrun += length - n;
		tty_flip_buffer_push(&bport->tty_port);
		tty_kref_put(tty);
		return;
	}

	n = kfifo_in_spinlocked(&bport->rxf, data, length, &bport->rx_lock);
	if (n < length) {
		bport->cnt_buf_overrun += length - n;
		pr_err_ratelimited(PFX "rx overrun, %d bytes dropped\n", length - n);
	}
	queue_work(bport->rx_wq, &bport->rx_work);
}

static void bcm4773_rx_work(struct work_struct *work)
{
	struct bcm4773_port *bport = container_of(work, struct bcm4773_port,
						  rx_work);
	unsigned char buf[512];
	unsigned int n;

	while ((n = kfifo_out_spinlocked(&bport->rxf, buf, sizeof(buf),
					 &bport->rx_lock)) > 0)
		bbd_ssi_spi_on_rx(buf, n);
}

/* ------------------------------------------------------------------------
 * IRQ / worker
 * --------------------------------------------------------------------- */

static void bcm4773_irq_off(struct bcm4773_port *bport)
{
	if (atomic_xchg(&bport->irq_enabled, 0))
		disable_irq_nosync(bport->irq);
}

static void bcm4773_irq_on(struct bcm4773_port *bport)
{
	if (!atomic_xchg(&bport->irq_enabled, 1))
		enable_irq(bport->irq);
}

static irqreturn_t bcm4773_irq_handler(int irq, void *pdata)
{
	struct bcm4773_port *bport = pdata;

	/* [FW4773-413] Check if we really have pending irq */
	if (!gpiod_get_value(bport->host_req))
		return IRQ_HANDLED;

	bcm4773_irq_off(bport);
	pm_wakeup_event(&bport->spi->dev, 100);
	queue_work(bport->wq, &bport->rxtx_work);
	return IRQ_HANDLED;
}

static void bcm4773_rxtx_work(struct work_struct *work)
{
	struct bcm4773_port *bport = container_of(work, struct bcm4773_port,
						  rxtx_work);
	int length = 0;
	unsigned int pending;
	int status;

	status = gpiod_get_value_cansleep(bport->host_req);
	pending = kfifo_len(&bport->txf);

	if (!status && !pending)
		goto out;

	if (!bcm4773_hello(bport)) {
		pr_err_ratelimited(PFX "%s timeout!!\n", __func__);
		goto out;
	}

	/* Drain the TX/RX buffer in a loop equally. */
	do {
		if (status) {
			status = bcm4773_read_transmit(bport, &length);
			if (BCM4773_MSG_STAT(status) != 0x00) {
				pr_err_ratelimited(PFX "read_transmit buffer error! retval=0x%04X\n",
						   status);
				break;
			} else if (BCM4773_MSG_LEN(status) != 0x00) {
				bcm4773_rx_insert(bport, bport->rx_msg->data + 1,
						  length);
			}
			/* keep checking HOST_REQ for the full-duplex write below */
			status = gpiod_get_value_cansleep(bport->host_req);
		}

		if (pending) {
			unsigned int n;

			if (pending > BCM4773_XMIT_SIZE - 1)
				pending = BCM4773_XMIT_SIZE - 1;

			/* Full duplex carries at most 255 bytes (+1 TX_PKT_LEN) */
			length = 1;
			if (pending > SSI_MAX_RW_BYTE_COUNT) {
				if (status) {
					pending = SSI_MAX_RW_BYTE_COUNT;
				} else {
					if (bpw32)
						pending -= (pending % (SPI_DMA_BYTES_PER_WORD *
								      WORD_BURST_SIZE)) + 1;
					length = 0;
				}
			}

			n = kfifo_out_spinlocked(&bport->txf,
						 bport->tx_msg->data + length,
						 pending, &bport->tx_lock);
			wake_up(&bport->tx_wait);
			if (use_tty)
				tty_port_tty_wakeup(&bport->tty_port);
			pending = n;

			if (pending) {
				if (length == 0) {
					status = bcm4773_transfer(bport, SSI_WRITE_TRANS |
								  SSI_MODE_HALF_DUPLEX,
								  pending);
					status = BCM4773_MSG_STAT(status);
				} else {
					status = bcm4773_write_receive(bport, pending,
								       &length);
				}

				if (BCM4773_MSG_STAT(status) == 0x00) {
					bport->cnt_tx += pending;
					if (length != 0)
						bcm4773_rx_insert(bport,
								  bport->rx_msg->data + 1,
								  length);
				} else {
					bport->cnt_overrun += pending;
				}
			}
		}

		status = gpiod_get_value_cansleep(bport->host_req);
		pending = kfifo_len(&bport->txf);
	} while ((pending || status) && !atomic_read(&bport->suspending));

	bcm4773_bye(bport);

out:
	if (bport->running && !atomic_read(&bport->suspending))
		bcm4773_irq_on(bport);
}

/*
 * Queue bytes for the chip.  Called by the BbdBridge (process context,
 * engine mutex held) and by the tty.  May sleep up to 1s for fifo space.
 */
int bcm4773_tx_push(const unsigned char *buf, size_t len, bool may_wait)
{
	struct bcm4773_port *bport = g_bport;
	size_t done = 0;

	if (!bport)
		return -ENODEV;

	while (done < len) {
		unsigned int n = kfifo_in_spinlocked(&bport->txf, buf + done,
						     len - done, &bport->tx_lock);
		done += n;
		queue_work(bport->wq, &bport->rxtx_work);
		if (done == len)
			break;
		if (!may_wait || current_work() == &bport->rxtx_work)
			break;
		if (!wait_event_timeout(bport->tx_wait,
					kfifo_avail(&bport->txf) > 0, HZ))
			break;
	}
	if (done < len)
		pr_err_ratelimited(PFX "tx fifo full, %zu bytes dropped\n",
				   len - done);
	return done;
}

/* ------------------------------------------------------------------------
 * SSI debug mode (used by /dev/bbd_ssi_spi_debug)
 * --------------------------------------------------------------------- */

unsigned char *bcm477x_debug_buffer(size_t *len)
{
	struct bcm4773_port *bport = g_bport;

	if (!bport || !len)
		return NULL;
	memset(bport->dbg_tx, 0xA5, sizeof(*bport->dbg_tx));
	*len = sizeof(*bport->dbg_tx);
	return (unsigned char *)bport->dbg_tx;
}

static int bcm4773_debug_xfer(struct bcm4773_port *bport, size_t length,
			      unsigned char *rx)
{
	unsigned char cmd_stat = bport->dbg_tx->cmd_stat;
	struct spi_transfer t = {
		.tx_buf = bport->dbg_tx,
		.rx_buf = bport->dbg_rx,
		.len = length,
		.bits_per_word = 8,
	};
	int status;

	if (length > sizeof(*bport->dbg_tx) || length == 0)
		return -EFAULT;

	if (bpw32 && cmd_stat == (SSI_MODE_HALF_DUPLEX | SSI_WRITE_TRANS | SSI_MODE_DEBUG) &&
	    length > SSI_MAX_RW_BYTE_COUNT &&
	    !(length % (SPI_DMA_BYTES_PER_WORD * WORD_BURST_SIZE)))
		t.bits_per_word = 32;

	pk_log(&bport->spi->dev, 'w', cmd_stat, bport->dbg_tx->data, length - 1);

	mutex_lock(&bport->spi_lock);
	status = spi_sync_transfer(bport->spi, &t, 1);
	mutex_unlock(&bport->spi_lock);
	if (status)
		return 0;
	if (rx)
		memcpy(rx, bport->dbg_rx, length);
	return length;
}

int bcm477x_debug_write(const unsigned char *buf, size_t len, int flag)
{
	struct bcm4773_port *bport = g_bport;

	if (!bport)
		return 0;
	if (buf != (unsigned char *)bport->dbg_tx)
		return -EFAULT;
	return bcm4773_debug_xfer(bport, len, NULL);
}

int bcm477x_debug_transfer(const unsigned char *tx, unsigned char *rx,
			   int len, int flag)
{
	struct bcm4773_port *bport = g_bport;

	if (!bport || len <= 0 || len > (int)sizeof(*bport->dbg_tx))
		return 0;
	memcpy(bport->dbg_tx, tx, len);
	return bcm4773_debug_xfer(bport, len, rx);
}

/* ------------------------------------------------------------------------
 * /dev/ttyBCM0
 * --------------------------------------------------------------------- */

static int bcm4773_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct bcm4773_port *bport = g_bport;

	if (!bport)
		return -ENODEV;
	tty->driver_data = bport;
	return tty_port_install(&bport->tty_port, driver, tty);
}

static int bcm4773_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct bcm4773_port *bport = tty->driver_data;

	return tty_port_open(&bport->tty_port, tty, filp);
}

static void bcm4773_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct bcm4773_port *bport = tty->driver_data;

	tty_port_close(&bport->tty_port, tty, filp);
}

static void bcm4773_tty_hangup(struct tty_struct *tty)
{
	struct bcm4773_port *bport = tty->driver_data;

	tty_port_hangup(&bport->tty_port);
}

static ssize_t bcm4773_tty_write(struct tty_struct *tty, const u8 *buf,
				 size_t count)
{
	return bcm4773_tx_push(buf, count, false);
}

static unsigned int bcm4773_tty_write_room(struct tty_struct *tty)
{
	struct bcm4773_port *bport = tty->driver_data;

	return kfifo_avail(&bport->txf);
}

static unsigned int bcm4773_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct bcm4773_port *bport = tty->driver_data;

	return kfifo_len(&bport->txf);
}

static void bcm4773_tty_set_termios(struct tty_struct *tty,
				    const struct ktermios *old)
{
	/* Termios field is fake. */
}

static const struct tty_operations bcm4773_tty_ops = {
	.install		= bcm4773_tty_install,
	.open			= bcm4773_tty_open,
	.close			= bcm4773_tty_close,
	.hangup			= bcm4773_tty_hangup,
	.write			= bcm4773_tty_write,
	.write_room		= bcm4773_tty_write_room,
	.chars_in_buffer	= bcm4773_tty_chars_in_buffer,
	.set_termios		= bcm4773_tty_set_termios,
};

static const struct tty_port_operations bcm4773_tty_port_ops = { };

static int bcm4773_tty_driver_init(void)
{
	struct tty_driver *drv;
	int ret;

	if (!use_tty)
		return 0;

	drv = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(drv))
		return PTR_ERR(drv);

	drv->driver_name = "ttyBCM";
	drv->name = "ttyBCM";
	drv->major = 0;
	drv->minor_start = 0;
	drv->type = TTY_DRIVER_TYPE_SERIAL;
	drv->subtype = SERIAL_TYPE_NORMAL;
	drv->init_termios = tty_std_termios;
	drv->init_termios.c_cflag = B921600 | CS8 | CREAD | CLOCAL;
	drv->init_termios.c_iflag = 0;
	drv->init_termios.c_oflag = 0;
	drv->init_termios.c_lflag = 0;
	tty_set_operations(drv, &bcm4773_tty_ops);

	ret = tty_register_driver(drv);
	if (ret) {
		tty_driver_kref_put(drv);
		return ret;
	}
	bcm4773_tty_driver = drv;
	return 0;
}

static void bcm4773_tty_driver_exit(void)
{
	if (!bcm4773_tty_driver)
		return;
	tty_unregister_driver(bcm4773_tty_driver);
	tty_driver_kref_put(bcm4773_tty_driver);
	bcm4773_tty_driver = NULL;
}

/* ------------------------------------------------------------------------
 * /sys/class/sec/gps/GPS_PWR_EN/value (downstream sec_gps_bcm47531.c
 * gpio_export_link), only if gps-pwr-en-gpios is in the DT node.
 * --------------------------------------------------------------------- */

static const struct class sec_class = {
	.name = "sec",
};
static bool sec_class_registered;

static ssize_t value_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct bcm4773_port *bport = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(bport->pwr_en));
}

static ssize_t value_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct bcm4773_port *bport = dev_get_drvdata(dev);
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	gpiod_set_value_cansleep(bport->pwr_en, on);
	return count;
}
static DEVICE_ATTR_RW(value);

static struct attribute *gps_pwr_attrs[] = {
	&dev_attr_value.attr,
	NULL
};
ATTRIBUTE_GROUPS(gps_pwr);

static void gps_pwr_release(struct device *dev)
{
	kfree(dev);
}

static void bcm4773_gps_power_init(struct bcm4773_port *bport)
{
	struct device *dev;
	int ret;

	if (!bport->pwr_en)
		return;

	if (!sec_class_registered) {
		if (class_register(&sec_class)) {
			dev_warn(&bport->spi->dev, "class 'sec' exists, no GPS_PWR_EN node\n");
			return;
		}
		sec_class_registered = true;
	}

	bport->gps_dev = device_create(&sec_class, NULL, 0, bport, "gps");
	if (IS_ERR(bport->gps_dev)) {
		bport->gps_dev = NULL;
		return;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		goto err;
	device_initialize(dev);
	dev->parent = bport->gps_dev;
	dev->groups = gps_pwr_groups;
	dev->release = gps_pwr_release;
	dev_set_drvdata(dev, bport);
	dev_set_name(dev, "GPS_PWR_EN");
	ret = device_add(dev);
	if (ret) {
		put_device(dev);
		goto err;
	}
	bport->pwr_dev = dev;
	return;
err:
	device_unregister(bport->gps_dev);
	bport->gps_dev = NULL;
}

static void bcm4773_gps_power_exit(struct bcm4773_port *bport)
{
	if (bport->pwr_dev)
		device_unregister(bport->pwr_dev);
	if (bport->gps_dev)
		device_unregister(bport->gps_dev);
	bport->pwr_dev = bport->gps_dev = NULL;
}

/* ------------------------------------------------------------------------
 * sysfs statistics on the SPI device
 * --------------------------------------------------------------------- */

static ssize_t ssi_stats_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct bcm4773_port *bport = spi_get_drvdata(to_spi_device(dev));

	return sysfs_emit(buf, "rx=%lu tx=%lu brk=%lu overrun=%lu buf_overrun=%lu txq=%u host_req=%d\n",
			  bport->cnt_rx, bport->cnt_tx, bport->cnt_brk,
			  bport->cnt_overrun, bport->cnt_buf_overrun,
			  kfifo_len(&bport->txf),
			  gpiod_get_value_cansleep(bport->host_req));
}
static DEVICE_ATTR_RO(ssi_stats);

/* ------------------------------------------------------------------------
 * probe / remove
 * --------------------------------------------------------------------- */

/* new-style name first ("host-req-gpios"), then downstream-style
 * ("ssp-irq-gpios"); returns NULL if neither exists */
static struct gpio_desc *bcm4773_gpiod_get(struct device *dev, const char *name,
					   const char *legacy,
					   enum gpiod_flags flags)
{
	struct gpio_desc *d = devm_gpiod_get_optional(dev, name, flags);

	if (!d)
		d = devm_gpiod_get_optional(dev, legacy, flags);
	return d;
}

static void bcm4773_free(struct bcm4773_port *bport)
{
	kfree(bport->tx_msg);
	kfree(bport->rx_msg);
	kfree(bport->dbg_tx);
	kfree(bport->dbg_rx);
	kfifo_free(&bport->txf);
	kfifo_free(&bport->rxf);
	if (bport->wq)
		destroy_workqueue(bport->wq);
	if (bport->rx_wq)
		destroy_workqueue(bport->rx_wq);
	kfree(bport);
}

static int bcm4773_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct bcm4773_port *bport;
	u32 hw_rev;
	int ret;

	if (g_bport)
		return -EBUSY;

	bport = kzalloc(sizeof(*bport), GFP_KERNEL);
	if (!bport)
		return -ENOMEM;
	bport->spi = spi;
	spin_lock_init(&bport->tx_lock);
	spin_lock_init(&bport->rx_lock);
	init_waitqueue_head(&bport->tx_wait);
	mutex_init(&bport->spi_lock);
	INIT_WORK(&bport->rxtx_work, bcm4773_rxtx_work);
	INIT_WORK(&bport->rx_work, bcm4773_rx_work);
	tty_port_init(&bport->tty_port);
	bport->tty_port.ops = &bcm4773_tty_port_ops;

	/* DMA-safe transfer buffers */
	bport->tx_msg = kmalloc(sizeof(*bport->tx_msg), GFP_KERNEL);
	bport->rx_msg = kmalloc(sizeof(*bport->rx_msg), GFP_KERNEL);
	bport->dbg_tx = kmalloc(sizeof(*bport->dbg_tx), GFP_KERNEL);
	bport->dbg_rx = kmalloc(sizeof(*bport->dbg_rx), GFP_KERNEL);
	if (!bport->tx_msg || !bport->rx_msg || !bport->dbg_tx || !bport->dbg_rx ||
	    kfifo_alloc(&bport->txf, BCM4773_TX_FIFO_SIZE, GFP_KERNEL) ||
	    kfifo_alloc(&bport->rxf, BCM4773_RX_FIFO_SIZE, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_free;
	}

	bport->wq = alloc_ordered_workqueue("bcm4773_wq", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	bport->rx_wq = alloc_ordered_workqueue("bcm4773_rx_wq", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!bport->wq || !bport->rx_wq) {
		ret = -ENOMEM;
		goto err_free;
	}

	/*
	 * HOST_REQ: the ASIC has data to send.  GPIOD_ASIS: setting the
	 * direction would switch the Exynos pin out of its EINT function
	 * (downstream never requested this gpio, it only read the level).
	 */
	bport->host_req = bcm4773_gpiod_get(dev, "host-req", "ssp-irq", GPIOD_ASIS);
	if (IS_ERR_OR_NULL(bport->host_req)) {
		ret = dev_err_probe(dev, bport->host_req ? PTR_ERR(bport->host_req) : -ENOENT,
				    "no host-req-gpios (HOST_REQ)\n");
		goto err_free;
	}
	bport->mcu_req = bcm4773_gpiod_get(dev, "mcu-req", "ssp-mcu-req", GPIOD_OUT_LOW);
	if (IS_ERR(bport->mcu_req)) {
		ret = PTR_ERR(bport->mcu_req);
		goto err_free;
	}
	bport->mcu_resp = bcm4773_gpiod_get(dev, "mcu-resp", "ssp-mcu-resp", GPIOD_IN);
	if (IS_ERR(bport->mcu_resp)) {
		ret = PTR_ERR(bport->mcu_resp);
		goto err_free;
	}
	if (!bport->mcu_req || !bport->mcu_resp)
		dev_warn(dev, "no MCU_REQ/MCU_RESP gpios, handshake disabled\n");
	/*
	 * nSTANDBY / GPS_PWR_EN (gpg2-0): downstream sec_gps_bcm47531.c drove it
	 * low at boot and LHD raises it.  Keep the current level so reloading
	 * the module does not power the hub off under a running LHD.
	 */
	bport->pwr_en = bcm4773_gpiod_get(dev, "enable", "gps-pwr-en", GPIOD_ASIS);
	if (IS_ERR(bport->pwr_en)) {
		ret = PTR_ERR(bport->pwr_en);
		goto err_free;
	}
	if (bport->pwr_en && gpiod_get_direction(bport->pwr_en) != 0)
		gpiod_direction_output(bport->pwr_en, 0);

	if (!device_property_read_u32(dev, "ssp-hw-rev", &hw_rev))
		gpbbd_dev->hw_rev = hw_rev;

	bport->irq = spi->irq > 0 ? spi->irq : gpiod_to_irq(bport->host_req);
	if (bport->irq < 0) {
		ret = dev_err_probe(dev, bport->irq, "no irq for HOST_REQ\n");
		goto err_free;
	}

	/* was done by ssp_probe() downstream */
	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "spi_setup failed %d\n", ret);
		goto err_free;
	}

	spi_set_drvdata(spi, bport);

	/* Reset FIFO */
	bcm4773_reset_fifo(bport);

	ret = request_irq(bport->irq, bcm4773_irq_handler,
			  IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, "bcm4773", bport);
	if (ret) {
		dev_err(dev, "failed to request irq %d\n", bport->irq);
		goto err_free;
	}
	atomic_set(&bport->irq_enabled, 0);
	atomic_set(&bport->suspending, 0);

	mutex_lock(&g_bport_lock);
	g_bport = bport;
	mutex_unlock(&g_bport_lock);

	if (use_tty && bcm4773_tty_driver) {
		bport->tty_dev = tty_port_register_device(&bport->tty_port,
							  bcm4773_tty_driver, 0, dev);
		if (IS_ERR(bport->tty_dev)) {
			dev_warn(dev, "no /dev/ttyBCM0 (%ld)\n", PTR_ERR(bport->tty_dev));
			bport->tty_dev = NULL;
		}
	}

	bcm4773_gps_power_init(bport);
	device_create_file(dev, &dev_attr_ssi_stats);

	device_init_wakeup(dev, true);
	bport->running = true;
	bcm4773_irq_on(bport);
	enable_irq_wake(bport->irq);

	/* SSP sensor hub: child device sharing our DT node (ssp_bbd.ko) */
	bport->ssp_pdev = platform_device_alloc("bbd-ssp", PLATFORM_DEVID_NONE);
	if (bport->ssp_pdev) {
		bport->ssp_pdev->dev.parent = dev;
		device_set_of_node_from_dev(&bport->ssp_pdev->dev, dev);
		if (platform_device_add(bport->ssp_pdev)) {
			platform_device_put(bport->ssp_pdev);
			bport->ssp_pdev = NULL;
		}
	}
	if (!bport->ssp_pdev)
		dev_warn(dev, "could not create the bbd-ssp device\n");

	dev_info(dev, "BCM4773 SPI/SSI driver v" GPS_VERSION ", irq %d\n", bport->irq);
	return 0;

err_free:
	tty_port_destroy(&bport->tty_port);
	bcm4773_free(bport);
	return ret;
}

static void bcm4773_spi_remove(struct spi_device *spi)
{
	struct bcm4773_port *bport = spi_get_drvdata(spi);

	if (bport->ssp_pdev)
		platform_device_unregister(bport->ssp_pdev);

	bport->running = false;
	disable_irq_wake(bport->irq);
	bcm4773_irq_off(bport);
	free_irq(bport->irq, bport);
	device_init_wakeup(&spi->dev, false);

	device_remove_file(&spi->dev, &dev_attr_ssi_stats);
	bcm4773_gps_power_exit(bport);

	if (bport->tty_dev) {
		tty_port_tty_hangup(&bport->tty_port, false);
		tty_unregister_device(bcm4773_tty_driver, 0);
	}

	cancel_work_sync(&bport->rxtx_work);
	cancel_work_sync(&bport->rx_work);

	mutex_lock(&g_bport_lock);
	g_bport = NULL;
	mutex_unlock(&g_bport_lock);
	wake_up_all(&bport->tx_wait);

	tty_port_destroy(&bport->tty_port);
	bcm4773_free(bport);
}

static int bcm4773_suspend(struct device *dev)
{
	struct bcm4773_port *bport = dev_get_drvdata(dev);

	atomic_set(&bport->suspending, 1);
	bcm4773_irq_off(bport);
	flush_workqueue(bport->wq);
	return 0;
}

static int bcm4773_resume(struct device *dev)
{
	struct bcm4773_port *bport = dev_get_drvdata(dev);

	atomic_set(&bport->suspending, 0);
	bcm4773_irq_on(bport);
	pm_wakeup_event(dev, 500);	/* downstream: wake_lock_timeout(HZ/2) */
	/* data may have been queued or announced while we slept */
	queue_work(bport->wq, &bport->rxtx_work);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(bcm4773_pm_ops, bcm4773_suspend, bcm4773_resume);

static const struct spi_device_id bcm4773_spi_id[] = {
	{ "ssp-spi", 0 },
	{ "BCM4773", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, bcm4773_spi_id);

static const struct of_device_id bcm4773_match_table[] = {
	{ .compatible = "ssp,BCM4773", },
	{ },
};
MODULE_DEVICE_TABLE(of, bcm4773_match_table);

static struct spi_driver bcm4773_spi_driver = {
	.id_table	= bcm4773_spi_id,
	.probe		= bcm4773_spi_probe,
	.remove		= bcm4773_spi_remove,
	.driver = {
		.name		= "ssp",
		.of_match_table	= bcm4773_match_table,
		.pm		= pm_sleep_ptr(&bcm4773_pm_ops),
		.suppress_bind_attrs = true,
	},
};

int bcm_gps_spi_init(void)
{
	int ret;

	ret = bcm4773_tty_driver_init();
	if (ret)
		pr_warn(PFX "ttyBCM driver not registered (%d)\n", ret);

	ret = spi_register_driver(&bcm4773_spi_driver);
	if (ret)
		bcm4773_tty_driver_exit();
	return ret;
}

void bcm_gps_spi_exit(void)
{
	spi_unregister_driver(&bcm4773_spi_driver);
	bcm4773_tty_driver_exit();
	if (sec_class_registered) {
		class_unregister(&sec_class);
		sec_class_registered = false;
	}
}
