/*
 * Copyright 2017 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define DEVICE_NAME	"aspeed-mbox"

#define ASPEED_MBOX_NUM_REGS 16

#define ASPEED_MBOX_DATA_0 0x00
#define ASPEED_MBOX_STATUS_0 0x40
#define ASPEED_MBOX_STATUS_1 0x44
#define ASPEED_MBOX_BMC_CTRL 0x48
#define   ASPEED_MBOX_CTRL_RECV BIT(7)
#define   ASPEED_MBOX_CTRL_MASK BIT(1)
#define   ASPEED_MBOX_CTRL_SEND BIT(0)
#define ASPEED_MBOX_HOST_CTRL 0x4c
#define ASPEED_MBOX_INTERRUPT_0 0x50
#define ASPEED_MBOX_INTERRUPT_1 0x54

struct aspeed_mbox {
	struct miscdevice	miscdev;
	struct regmap		*regmap;
	unsigned int		base;
	wait_queue_head_t	queue;
	struct mutex		mutex;
};

static atomic_t aspeed_mbox_open_count = ATOMIC_INIT(0);

static u8 aspeed_mbox_inb(struct aspeed_mbox *mbox, int reg)
{
	/*
	 * The mbox registers are actually only one byte but are addressed
	 * four bytes apart. The other three bytes are marked 'reserved',
	 * they *should* be zero but lets not rely on it.
	 * I am going to rely on the fact we can casually read/write to them...
	 */
	unsigned int val = 0xff; /* If regmap throws an error return 0xff */
	int rc = regmap_read(mbox->regmap, mbox->base + reg, &val);

	if (rc)
		dev_err(mbox->miscdev.parent, "regmap_read() failed with "
				"%d (reg: 0x%08x)\n", rc, reg);

	return val & 0xff;
}

static void aspeed_mbox_outb(struct aspeed_mbox *mbox, u8 data, int reg)
{
	int rc = regmap_write(mbox->regmap, mbox->base + reg, data);

	if (rc)
		dev_err(mbox->miscdev.parent, "regmap_write() failed with "
				"%d (data: %u reg: 0x%08x)\n", rc, data, reg);
}

static struct aspeed_mbox *file_mbox(struct file *file)
{
	return container_of(file->private_data, struct aspeed_mbox, miscdev);
}

static int aspeed_mbox_open(struct inode *inode, struct file *file)
{
	struct aspeed_mbox *mbox = file_mbox(file);

	if (atomic_inc_return(&aspeed_mbox_open_count) == 1) {
		/*
		 * Clear the interrupt status bit if it was left on and unmask
		 * interrupts.
		 * ASPEED_MBOX_CTRL_RECV bit is W1C, this also unmasks in 1 step
		 */
		aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_RECV, ASPEED_MBOX_BMC_CTRL);
		return 0;
	}

	atomic_dec(&aspeed_mbox_open_count);
	return -EBUSY;
}

static ssize_t aspeed_mbox_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct aspeed_mbox *mbox = file_mbox(file);
	char __user *p = buf;
	ssize_t ret;
	int i;

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	if (count + *ppos > ASPEED_MBOX_NUM_REGS)
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK) {
		if (!(aspeed_mbox_inb(mbox, ASPEED_MBOX_BMC_CTRL) &
				ASPEED_MBOX_CTRL_RECV))
			return -EAGAIN;
	} else if (wait_event_interruptible(mbox->queue,
				aspeed_mbox_inb(mbox, ASPEED_MBOX_BMC_CTRL) &
				ASPEED_MBOX_CTRL_RECV)) {
		return -ERESTARTSYS;
	}

	mutex_lock(&mbox->mutex);

	for (i = *ppos; count > 0 && i < ASPEED_MBOX_NUM_REGS; i++) {
		uint8_t reg = aspeed_mbox_inb(mbox, ASPEED_MBOX_DATA_0 + (i * 4));

		ret = __put_user(reg, p);
		if (ret)
			goto out_unlock;

		p++;
		count--;
	}

	/* ASPEED_MBOX_CTRL_RECV bit is write to clear, this also unmasks in 1 step */
	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_RECV, ASPEED_MBOX_BMC_CTRL);
	ret = p - buf;

out_unlock:
	mutex_unlock(&mbox->mutex);
	return ret;
}

static ssize_t aspeed_mbox_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct aspeed_mbox *mbox = file_mbox(file);
	const char __user *p = buf;
	ssize_t ret;
	char c;
	int i;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	if (count + *ppos > ASPEED_MBOX_NUM_REGS)
		return -EINVAL;

	mutex_lock(&mbox->mutex);

	for (i = *ppos; count > 0 && i < ASPEED_MBOX_NUM_REGS; i++) {
		ret = __get_user(c, p);
		if (ret)
			goto out_unlock;

		aspeed_mbox_outb(mbox, c, ASPEED_MBOX_DATA_0 + (i * 4));
		p++;
		count--;
	}

	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_SEND, ASPEED_MBOX_BMC_CTRL);
	ret = p - buf;

out_unlock:
	mutex_unlock(&mbox->mutex);
	return ret;
}

static unsigned int aspeed_mbox_poll(struct file *file, poll_table *wait)
{
	struct aspeed_mbox *mbox = file_mbox(file);
	unsigned int mask = 0;

	poll_wait(file, &mbox->queue, wait);

	if (aspeed_mbox_inb(mbox, ASPEED_MBOX_BMC_CTRL) & ASPEED_MBOX_CTRL_RECV)
		mask |= POLLIN;

	return mask;
}

static int aspeed_mbox_release(struct inode *inode, struct file *file)
{
	atomic_dec(&aspeed_mbox_open_count);
	return 0;
}

static const struct file_operations aspeed_mbox_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_seek_end_llseek,
	.read		= aspeed_mbox_read,
	.write		= aspeed_mbox_write,
	.open		= aspeed_mbox_open,
	.release	= aspeed_mbox_release,
	.poll		= aspeed_mbox_poll,
};

static irqreturn_t aspeed_mbox_irq(int irq, void *arg)
{
	struct aspeed_mbox *mbox = arg;

	if (!(aspeed_mbox_inb(mbox, ASPEED_MBOX_BMC_CTRL) & ASPEED_MBOX_CTRL_RECV))
		return IRQ_NONE;

	/*
	 * Leave the status bit set so that we know the data is for us,
	 * clear it once it has been read.
	 */

	/* Mask it off, we'll clear it when we the data gets read */
	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_MASK, ASPEED_MBOX_BMC_CTRL);

	wake_up(&mbox->queue);
	return IRQ_HANDLED;
}

static int aspeed_mbox_config_irq(struct aspeed_mbox *mbox,
		struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc, irq;

	irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!irq)
		return -ENODEV;

	rc = devm_request_irq(dev, irq, aspeed_mbox_irq,
			IRQF_SHARED, DEVICE_NAME, mbox);
	if (rc < 0) {
		dev_err(dev, "Unable to request IRQ %d\n", irq);
		return rc;
	}

	/*
	 * Disable all register based interrupts.
	 */
	aspeed_mbox_outb(mbox, 0x00, ASPEED_MBOX_INTERRUPT_0); /* regs 0 - 7 */
	aspeed_mbox_outb(mbox, 0x00, ASPEED_MBOX_INTERRUPT_1); /* regs 8 - 15 */

	/* These registers are write one to clear. Clear them. */
	aspeed_mbox_outb(mbox, 0xff, ASPEED_MBOX_STATUS_0);
	aspeed_mbox_outb(mbox, 0xff, ASPEED_MBOX_STATUS_1);

	aspeed_mbox_outb(mbox, ASPEED_MBOX_CTRL_RECV, ASPEED_MBOX_BMC_CTRL);
	return 0;
}

static int aspeed_mbox_probe(struct platform_device *pdev)
{
	struct aspeed_mbox *mbox;
	struct device *dev;
	int rc;

	dev = &pdev->dev;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, mbox);

	rc = of_property_read_u32(dev->of_node, "reg", &mbox->base);
	if (rc) {
		dev_err(dev, "Couldn't read reg device-tree property\n");
		return rc;
	}

	mbox->regmap = syscon_node_to_regmap(
			pdev->dev.parent->of_node);
	if (IS_ERR(mbox->regmap)) {
		dev_err(dev, "Couldn't get regmap\n");
		return -ENODEV;
	}

	mutex_init(&mbox->mutex);
	init_waitqueue_head(&mbox->queue);

	mbox->miscdev.minor = MISC_DYNAMIC_MINOR;
	mbox->miscdev.name = DEVICE_NAME;
	mbox->miscdev.fops = &aspeed_mbox_fops;
	mbox->miscdev.parent = dev;
	rc = misc_register(&mbox->miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		return rc;
	}

	rc = aspeed_mbox_config_irq(mbox, pdev);
	if (rc) {
		dev_err(dev, "Failed to configure IRQ\n");
		misc_deregister(&mbox->miscdev);
		return rc;
	}

	return 0;
}

static int aspeed_mbox_remove(struct platform_device *pdev)
{
	struct aspeed_mbox *mbox = dev_get_drvdata(&pdev->dev);

	misc_deregister(&mbox->miscdev);

	return 0;
}

static const struct of_device_id aspeed_mbox_match[] = {
	{ .compatible = "aspeed,ast2400-mbox" },
	{ .compatible = "aspeed,ast2500-mbox" },
	{ },
};

static struct platform_driver aspeed_mbox_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = aspeed_mbox_match,
	},
	.probe = aspeed_mbox_probe,
	.remove = aspeed_mbox_remove,
};

module_platform_driver(aspeed_mbox_driver);

MODULE_DEVICE_TABLE(of, aspeed_mbox_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Bur <cyrilbur@gmail.com>");
MODULE_DESCRIPTION("Aspeed mailbox device driver");
