// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/stpgps: the GPS channel of the CONSYS STP link. Opening it turns the
 * GPS function on, closing it turns it off again; in between, read() and
 * write() carry the raw stream between the positioning engine in userspace
 * and the GPS firmware.
 *
 * Based on MediaTek's stp_chrdev_gps.c.
 * Copyright (C) 2010 MediaTek Inc.
 */

#define pr_fmt(fmt) "[GPS] " fmt

#include <linux/cdev.h>
#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "osal_typedef.h"
#include "stp_exp.h"
#include "wmt_exp.h"

#define GPS_DRIVER_NAME		"mtk_stp_GPS_chrdev"
#define GPS_DEV_MAJOR		191

/* The ioctl numbers MediaTek's mnld uses on this generation: plain, no _IO() */
#define COMBO_IOC_GPS_HWVER	6
#define COMBO_IOC_RTC_FLAG	7
#define COMBO_IOC_CO_CLOCK_FLAG	8

/* The STP parser drops frames with a longer payload. */
#define GPS_TX_MAX		2048

static int gps_major = GPS_DEV_MAJOR;
module_param(gps_major, int, 0444);

static struct cdev gps_cdev;

static DEFINE_MUTEX(gps_open_lock);
static bool gps_opened;

static DEFINE_MUTEX(gps_rd_lock);
static DEFINE_MUTEX(gps_wr_lock);
static u8 gps_rx_buf[MTKSTP_BUFFER_SIZE];
static u8 gps_tx_buf[GPS_TX_MAX];

static DECLARE_WAIT_QUEUE_HEAD(gps_wq);

/*
 * Set by a whole-chip reset, which turns the GPS function off under the
 * opener; the stream stays dead until it closes and opens again.
 */
static bool gps_reset;

static bool gps_rx_ready(void)
{
	return READ_ONCE(gps_reset) || !mtk_wcn_stp_is_rxqueue_empty(GPS_TASK_INDX);
}

static void gps_event_cb(void)
{
	wake_up_interruptible(&gps_wq);
}

static void gps_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
		       ENUM_WMTMSG_TYPE_T type, PVOID buf, UINT32 sz)
{
	if (type != WMTMSG_TYPE_RESET ||
	    *(ENUM_WMTRSTMSG_TYPE_T *)buf != WMTRSTMSG_RESET_START)
		return;

	pr_warn("whole chip reset, the GPS function is off\n");
	WRITE_ONCE(gps_reset, true);
	wake_up_interruptible(&gps_wq);
}

static ssize_t gps_read(struct file *filp, char __user *buf, size_t count,
			loff_t *f_pos)
{
	int len;

	guard(mutex)(&gps_rd_lock);

	count = min(count, sizeof(gps_rx_buf));

	for (;;) {
		if (READ_ONCE(gps_reset))
			return -EIO;

		len = mtk_wcn_stp_receive_data(gps_rx_buf, count, GPS_TASK_INDX);
		if (len > 0)
			break;

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(gps_wq, gps_rx_ready()))
			return -ERESTARTSYS;
	}

	if (copy_to_user(buf, gps_rx_buf, len))
		return -EFAULT;

	return len;
}

static ssize_t gps_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *f_pos)
{
	int written;

	guard(mutex)(&gps_wr_lock);

	if (READ_ONCE(gps_reset))
		return -EIO;

	if (!count)
		return 0;

	count = min(count, sizeof(gps_tx_buf));
	if (copy_from_user(gps_tx_buf, buf, count))
		return -EFAULT;

	written = mtk_wcn_stp_send_data(gps_tx_buf, count, GPS_TASK_INDX);
	if (written < 0)
		return -EIO;
	if (!written)
		return -ENOSPC;	/* no room in the STP window, try again later */

	return written;
}

static __poll_t gps_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(filp, &gps_wq, wait);

	if (READ_ONCE(gps_reset))
		return mask | EPOLLERR;

	if (!mtk_wcn_stp_is_rxqueue_empty(GPS_TASK_INDX))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static long gps_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	ENUM_WMTHWVER_TYPE_T hw_ver;

	switch (cmd) {
	case COMBO_IOC_GPS_HWVER:
		hw_ver = mtk_wcn_wmt_hwver_get();
		if (copy_to_user((void __user *)arg, &hw_ver, sizeof(hw_ver)))
			return -EFAULT;
		return 0;

	case COMBO_IOC_RTC_FLAG:
		/*
		 * 1 tells the engine the RTC lost power and its time is not
		 * to be trusted. Nothing on mainline records that, so report
		 * the RTC as kept, as MediaTek's later drivers do too.
		 */
		return 0;

	case COMBO_IOC_CO_CLOCK_FLAG:
		return mtk_wcn_wmt_co_clock_flag_get();

	default:
		return -ENOTTY;
	}
}

/*
 * GPS streams once a second, which under the 30 ms PSM idle timer is a
 * sleep/wake handshake every second for as long as the receiver is on. Keep
 * the chip awake while the node is open, as Wi-Fi does while busy; PSM goes
 * back to its policy on close.
 */
static int gps_open(struct inode *inode, struct file *filp)
{
	guard(mutex)(&gps_open_lock);

	/* One stream, one reader: a second opener would steal its data. */
	if (gps_opened)
		return -EBUSY;

	mtk_wcn_wmt_psm_hold();

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_GPS) == MTK_WCN_BOOL_FALSE) {
		pr_err("WMT turn on GPS failed\n");
		mtk_wcn_wmt_psm_release();
		return -ENODEV;
	}

	if (!mtk_wcn_stp_is_ready()) {
		pr_err("STP is not ready\n");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_GPS);
		mtk_wcn_wmt_psm_release();
		return -ENODEV;
	}

	WRITE_ONCE(gps_reset, false);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_GPS, gps_rst_cb);
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, gps_event_cb);
	gps_opened = true;

	pr_info("GPS on (pid %d)\n", current->pid);

	return stream_open(inode, filp);
}

static int gps_release(struct inode *inode, struct file *filp)
{
	guard(mutex)(&gps_open_lock);

	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_GPS);

	/* After a reset the function is already off; WMT just says so. */
	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_GPS) == MTK_WCN_BOOL_FALSE)
		pr_err("WMT turn off GPS failed\n");
	else
		pr_info("GPS off\n");

	mtk_wcn_wmt_psm_release();
	gps_opened = false;

	return 0;
}

static const struct file_operations gps_fops = {
	.owner = THIS_MODULE,
	.open = gps_open,
	.release = gps_release,
	.read = gps_read,
	.write = gps_write,
	.poll = gps_poll,
	.unlocked_ioctl = gps_ioctl,
};

static int __init gps_init(void)
{
	dev_t dev = MKDEV(gps_major, 0);
	int ret;

	ret = register_chrdev_region(dev, 1, GPS_DRIVER_NAME);
	if (ret) {
		pr_err("cannot register major %d: %d\n", gps_major, ret);
		return ret;
	}

	cdev_init(&gps_cdev, &gps_fops);
	ret = cdev_add(&gps_cdev, dev, 1);
	if (ret) {
		unregister_chrdev_region(dev, 1);
		return ret;
	}

	pr_info("%s driver (major %d) installed\n", GPS_DRIVER_NAME, gps_major);

	return 0;
}

static void __exit gps_exit(void)
{
	cdev_del(&gps_cdev);
	unregister_chrdev_region(MKDEV(gps_major, 0), 1);
}

module_init(gps_init);
module_exit(gps_exit);

MODULE_DESCRIPTION("MediaTek CONSYS GPS character device");
MODULE_LICENSE("GPL");
