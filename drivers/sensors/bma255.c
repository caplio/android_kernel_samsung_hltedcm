/*
 * Copyright (C) 2013 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>

#include "sensors_core.h"
#include "bma255_reg.h"

/* It's for HW issue in Rev 0.4 */
#define EXECPTION_FOR_I2CFAIL

#define VENDOR_NAME                     "BOSCH"
#define MODEL_NAME                      "BMA255"
#define MODULE_NAME                     "accelerometer_sensor"

#define CALIBRATION_FILE_PATH           "/efs/accel_calibration_data"
#define CALIBRATION_DATA_AMOUNT         20
#define MAX_ACCEL_1G			1024

#define BMA255_DEFAULT_DELAY            200
#define BMA255_CHIP_ID                  0xFA

#define BMA255_TOP_UPPER_RIGHT          0
#define BMA255_TOP_LOWER_RIGHT          1
#define BMA255_TOP_LOWER_LEFT           2
#define BMA255_TOP_UPPER_LEFT           3
#define BMA255_BOTTOM_UPPER_RIGHT       4
#define BMA255_BOTTOM_LOWER_RIGHT       5
#define BMA255_BOTTOM_LOWER_LEFT        6
#define BMA255_BOTTOM_UPPER_LEFT        7

struct bma255_v {
	union {
		s16 v[3];
		struct {
			s16 x;
			s16 y;
			s16 z;
		};
	};
};

struct bma255_p {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct device *factory_device;
	struct bma255_v accdata;
	struct bma255_v caldata;

	atomic_t delay;
	atomic_t enable;

	u32 chip_pos;
	int acc_int1;
	int acc_int2;
#ifdef EXECPTION_FOR_I2CFAIL
	int i2cfail_cnt;
#endif
};

static int bma255_open_calibration(struct bma255_p *);

static int bma255_smbus_read_byte_block(struct i2c_client *client,
		unsigned char reg_addr, unsigned char *data, unsigned char len)
{
	s32 dummy;

	dummy = i2c_smbus_read_i2c_block_data(client, reg_addr, len, data);
	if (dummy < 0) {
		pr_err("[SENSOR]: %s - i2c bus read error %d\n",
			__func__, dummy);
		return -EIO;
	}
	return 0;
}

static int bma255_smbus_read_byte(struct i2c_client *client,
		unsigned char reg_addr, unsigned char *buf)
{
	s32 dummy;

	dummy = i2c_smbus_read_byte_data(client, reg_addr);
	if (dummy < 0) {
		pr_err("[SENSOR]: %s - i2c bus read error %d\n",
			__func__, dummy);
		return -EIO;
	}
	*buf = dummy & 0x000000ff;

	return 0;
}

static int bma255_smbus_write_byte(struct i2c_client *client,
		unsigned char reg_addr, unsigned char *buf)
{
	s32 dummy;

	dummy = i2c_smbus_write_byte_data(client, reg_addr, *buf);
	if (dummy < 0) {
		pr_err("[SENSOR]: %s - i2c bus read error %d\n",
			__func__, dummy);
		return -EIO;
	}
	return 0;
}

static int bma255_set_mode(struct bma255_p *data, unsigned char mode)
{
	int ret = 0;
	unsigned char buf1, buf2;

	ret = bma255_smbus_read_byte(data->client,
			BMA255_MODE_CTRL_REG, &buf1);
	ret += bma255_smbus_read_byte(data->client,
			BMA255_LOW_NOISE_CTRL_REG, &buf2);

	switch (mode) {
	case BMA255_MODE_NORMAL:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 0);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 0);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		break;
	case BMA255_MODE_LOWPOWER1:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 2);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 0);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		break;
	case BMA255_MODE_SUSPEND:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 4);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 0);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		break;
	case BMA255_MODE_DEEP_SUSPEND:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 1);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		break;
	case BMA255_MODE_LOWPOWER2:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 2);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		break;
	case BMA255_MODE_STANDBY:
		buf1  = BMA255_SET_BITSLICE(buf1, BMA255_MODE_CTRL, 4);
		buf2  = BMA255_SET_BITSLICE(buf2, BMA255_LOW_POWER_MODE, 1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_LOW_NOISE_CTRL_REG, &buf2);
		mdelay(1);
		ret += bma255_smbus_write_byte(data->client,
				BMA255_MODE_CTRL_REG, &buf1);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int bma255_set_range(struct bma255_p *data, unsigned char range)
{
	int ret = 0 ;
	unsigned char buf;

	ret = bma255_smbus_read_byte(data->client,
			BMA255_RANGE_SEL_REG, &buf);
	switch (range) {
	case BMA255_RANGE_2G:
		buf = BMA255_SET_BITSLICE(buf, BMA255_RANGE_SEL, 3);
		break;
	case BMA255_RANGE_4G:
		buf = BMA255_SET_BITSLICE(buf, BMA255_RANGE_SEL, 5);
		break;
	case BMA255_RANGE_8G:
		buf = BMA255_SET_BITSLICE(buf, BMA255_RANGE_SEL, 8);
		break;
	case BMA255_RANGE_16G:
		buf = BMA255_SET_BITSLICE(buf, BMA255_RANGE_SEL, 12);
		break;
	default:
		buf = BMA255_SET_BITSLICE(buf, BMA255_RANGE_SEL, 3);
		break;
	}

	ret += bma255_smbus_write_byte(data->client,
			BMA255_RANGE_SEL_REG, &buf);

	return ret;
}

static int bma255_set_bandwidth(struct bma255_p *data, unsigned char bandwidth)
{
	int ret = 0;
	unsigned char buf;

	if (bandwidth <= 7 || bandwidth >= 16)
		bandwidth = BMA255_BW_1000HZ;

	ret = bma255_smbus_read_byte(data->client, BMA255_BANDWIDTH__REG, &buf);
	buf = BMA255_SET_BITSLICE(buf, BMA255_BANDWIDTH, bandwidth);
	ret += bma255_smbus_write_byte(data->client,
			BMA255_BANDWIDTH__REG, &buf);

	return ret;
}

static int bma255_read_accel_xyz(struct bma255_p *data,	struct bma255_v *acc)
{
	int ret = 0;
	unsigned char buf[6];

	ret = bma255_smbus_read_byte_block(data->client,
			BMA255_ACC_X12_LSB__REG, buf, 6);

	acc->x = BMA255_GET_BITSLICE(buf[0], BMA255_ACC_X12_LSB) |
			(BMA255_GET_BITSLICE(buf[1], BMA255_ACC_X_MSB) <<
				(BMA255_ACC_X12_LSB__LEN));
	acc->x = acc->x << (sizeof(short) * 8 - (BMA255_ACC_X12_LSB__LEN +
				BMA255_ACC_X_MSB__LEN));
	acc->x = acc->x >> (sizeof(short) * 8 - (BMA255_ACC_X12_LSB__LEN +
				BMA255_ACC_X_MSB__LEN));

	acc->y = BMA255_GET_BITSLICE(buf[2], BMA255_ACC_Y12_LSB) |
			(BMA255_GET_BITSLICE(buf[3],
			BMA255_ACC_Y_MSB) << (BMA255_ACC_Y12_LSB__LEN));
	acc->y = acc->y << (sizeof(short) * 8 - (BMA255_ACC_Y12_LSB__LEN +
			BMA255_ACC_Y_MSB__LEN));
	acc->y = acc->y >> (sizeof(short) * 8 - (BMA255_ACC_Y12_LSB__LEN +
			BMA255_ACC_Y_MSB__LEN));

	acc->z = BMA255_GET_BITSLICE(buf[4], BMA255_ACC_Z12_LSB) |
			(BMA255_GET_BITSLICE(buf[5],
			BMA255_ACC_Z_MSB) << (BMA255_ACC_Z12_LSB__LEN));
	acc->z = acc->z << (sizeof(short) * 8 - (BMA255_ACC_Z12_LSB__LEN +
			BMA255_ACC_Z_MSB__LEN));
	acc->z = acc->z >> (sizeof(short) * 8 - (BMA255_ACC_Z12_LSB__LEN +
			BMA255_ACC_Z_MSB__LEN));

	remap_sensor_data(acc->v, data->chip_pos);

	return ret;
}

static void bma255_work_func(struct work_struct *work)
{
	struct bma255_v acc;
	struct bma255_p *data = container_of((struct delayed_work *)work,
			struct bma255_p, work);
	unsigned long delay = msecs_to_jiffies(atomic_read(&data->delay));
#ifdef EXECPTION_FOR_I2CFAIL
	int ret;

	ret = bma255_read_accel_xyz(data, &acc);
	if (ret < 0)
		data->i2cfail_cnt++;
	if (data->i2cfail_cnt > 5)
		return;
#else
	bma255_read_accel_xyz(data, &acc);
#endif
	input_report_rel(data->input, REL_X, acc.x - data->caldata.x);
	input_report_rel(data->input, REL_Y, acc.y - data->caldata.y);
	input_report_rel(data->input, REL_Z, acc.z - data->caldata.z);
	input_sync(data->input);
	data->accdata = acc;

	schedule_delayed_work(&data->work, delay);
}

static void bma255_set_enable(struct bma255_p *data, int enable)
{
	int pre_enable = atomic_read(&data->enable);

	if (enable) {
		if (pre_enable == 0) {
			bma255_open_calibration(data);
			bma255_set_mode(data, BMA255_MODE_NORMAL);
			schedule_delayed_work(&data->work,
				msecs_to_jiffies(atomic_read(&data->delay)));
			atomic_set(&data->enable, 1);
		}
	} else {
		if (pre_enable == 1) {
			bma255_set_mode(data, BMA255_MODE_SUSPEND);
			cancel_delayed_work_sync(&data->work);
			atomic_set(&data->enable, 0);
		}
	}
}

static ssize_t bma255_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bma255_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->enable));
}

static ssize_t bma255_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	u8 enable;
	int ret;
	struct bma255_p *data = dev_get_drvdata(dev);

	ret = kstrtou8(buf, 2, &enable);
	if (ret) {
		pr_err("[SENSOR]: %s - Invalid Argument\n", __func__);
		return ret;
	}

	pr_info("[SENSOR]: %s - new_value = %u\n", __func__, enable);
	if ((enable == 0) || (enable == 1))
		bma255_set_enable(data, (int)enable);

	return size;
}

static ssize_t bma255_delay_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bma255_p *data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&data->delay));
}

static ssize_t bma255_delay_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int ret;
	int64_t delay;
	struct bma255_p *data = dev_get_drvdata(dev);

	ret = kstrtoll(buf, 10, &delay);
	if (ret) {
		pr_err("[SENSOR]: %s - Invalid Argument\n", __func__);
		return ret;
	}

	atomic_set(&data->delay, (unsigned int)delay);
	pr_info("[SENSOR]: %s - poll_delay = %lld\n", __func__, delay);

	return size;
}

static DEVICE_ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
		bma255_delay_show, bma255_delay_store);
static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
		bma255_enable_show, bma255_enable_store);

static struct attribute *bma255_attributes[] = {
	&dev_attr_poll_delay.attr,
	&dev_attr_enable.attr,
	NULL
};


static struct attribute_group bma255_attribute_group = {
	.attrs = bma255_attributes
};


static ssize_t bma255_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}

static ssize_t bma255_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static int bma255_open_calibration(struct bma255_p *data)
{
	int ret = 0;
	mm_segment_t old_fs;
	struct file *cal_filp = NULL;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH, O_RDONLY, 0666);
	if (IS_ERR(cal_filp)) {
		set_fs(old_fs);
		ret = PTR_ERR(cal_filp);

		data->caldata.x = 0;
		data->caldata.y = 0;
		data->caldata.z = 0;

		pr_err("[SENSOR]: %s - cal_filp open failed(%d)\n",
			__func__, ret);

		return ret;
	}

	ret = cal_filp->f_op->read(cal_filp, (char *)&data->caldata,
		3 * sizeof(int), &cal_filp->f_pos);
	if (ret != 3 * sizeof(int))
		ret = -EIO;

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	pr_info("[SENSOR]: open accel calibration %d, %d, %d\n",
		data->caldata.x, data->caldata.y, data->caldata.z);

	if ((data->caldata.x == 0) && (data->caldata.y == 0)
		&& (data->caldata.z == 0))
		return -EIO;

	return ret;
}

static int bma255_do_calibrate(struct bma255_p *data, int enable)
{
	int sum[3] = { 0, };
	int ret = 0, cnt;
	struct file *cal_filp = NULL;
	struct bma255_v acc;
	mm_segment_t old_fs;

	if (enable) {
		data->caldata.x = 0;
		data->caldata.y = 0;
		data->caldata.z = 0;

		if (atomic_read(&data->enable) == 1)
			cancel_delayed_work_sync(&data->work);
		else
			bma255_set_mode(data, BMA255_MODE_NORMAL);

		msleep(300);

		for (cnt = 0; cnt < CALIBRATION_DATA_AMOUNT; cnt++) {
			bma255_read_accel_xyz(data, &acc);
			sum[0] += acc.x;
			sum[1] += acc.y;
			sum[2] += acc.z;
			mdelay(10);
		}

		if (atomic_read(&data->enable) == 1)
			schedule_delayed_work(&data->work,
				msecs_to_jiffies(atomic_read(&data->delay)));
		else
			bma255_set_mode(data, BMA255_MODE_SUSPEND);

		data->caldata.x = (sum[0] / CALIBRATION_DATA_AMOUNT);
		data->caldata.y = (sum[1] / CALIBRATION_DATA_AMOUNT);
		data->caldata.z = (sum[2] / CALIBRATION_DATA_AMOUNT);

		if (data->caldata.z > 0)
			data->caldata.z -= MAX_ACCEL_1G;
		else if (data->caldata.z < 0)
			data->caldata.z += MAX_ACCEL_1G;
	} else {
		data->caldata.x = 0;
		data->caldata.y = 0;
		data->caldata.z = 0;
	}

	pr_info("[SENSOR]: %s - do accel calibrate %d, %d, %d\n", __func__,
		data->caldata.x, data->caldata.y, data->caldata.z);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH,
			O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (IS_ERR(cal_filp)) {
		pr_err("[SENSOR]: %s - Can't open calibration file\n",
			__func__);
		set_fs(old_fs);
		ret = PTR_ERR(cal_filp);
		return ret;
	}

	ret = cal_filp->f_op->write(cal_filp, (char *)&data->caldata,
		3 * sizeof(int), &cal_filp->f_pos);
	if (ret != 3 * sizeof(int)) {
		pr_err("[SENSOR]: %s - Can't write the caldata to file\n",
			__func__);
		ret = -EIO;
	}

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	return ret;
}

static ssize_t bma255_calibration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	struct bma255_p *data = dev_get_drvdata(dev);

	ret = bma255_open_calibration(data);
	if (ret < 0)
		pr_err("[SENSOR]: %s - calibration open failed(%d)\n",
			__func__, ret);

	pr_info("[SENSOR]: %s - cal data %d %d %d - ret : %d\n", __func__,
		data->caldata.x, data->caldata.y, data->caldata.z, ret);

	return snprintf(buf, PAGE_SIZE, "%d %d %d %d\n", ret, data->caldata.x,
			data->caldata.y, data->caldata.z);
}

static ssize_t bma255_calibration_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int ret;
	int64_t dEnable;
	struct bma255_p *data = dev_get_drvdata(dev);

	ret = kstrtoll(buf, 10, &dEnable);
	if (ret < 0)
		return ret;

	ret = bma255_do_calibrate(data, (int)dEnable);
	if (ret < 0)
		pr_err("[SENSOR]: %s - accel calibrate failed\n", __func__);

	return size;
}

static ssize_t bma255_raw_data_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct bma255_v acc;
	struct bma255_p *data = dev_get_drvdata(dev);

	if (atomic_read(&data->enable) == 0) {
		bma255_set_mode(data, BMA255_MODE_NORMAL);
		msleep(20);
		bma255_read_accel_xyz(data, &acc);
		bma255_set_mode(data, BMA255_MODE_SUSPEND);
	} else {
		acc = data->accdata;
	}

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
			acc.x - data->caldata.x,
			acc.y - data->caldata.y,
			acc.z - data->caldata.z);
}

static ssize_t bma255_reactive_alert_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	if (sysfs_streq(buf, "1"))
		pr_err("[SENSOR]: %s - on\n", __func__);
	else if (sysfs_streq(buf, "0"))
		pr_err("[SENSOR]: %s - off\n", __func__);
	else if (sysfs_streq(buf, "2"))
		pr_err("[SENSOR]: %s - factory\n", __func__);

	return size;
}

static ssize_t bma255_reactive_alert_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	bool bSuccess = false;

	return snprintf(buf, PAGE_SIZE, "%u\n", bSuccess);
}

static DEVICE_ATTR(name, S_IRUGO, bma255_name_show, NULL);
static DEVICE_ATTR(vendor, S_IRUGO, bma255_vendor_show, NULL);
static DEVICE_ATTR(calibration, S_IRUGO | S_IWUSR | S_IWGRP,
	bma255_calibration_show, bma255_calibration_store);
static DEVICE_ATTR(raw_data, S_IRUGO, bma255_raw_data_read, NULL);
static DEVICE_ATTR(reactive_alert, S_IRUGO | S_IWUSR | S_IWGRP,
	bma255_reactive_alert_show, bma255_reactive_alert_store);

static struct device_attribute *sensor_attrs[] = {
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_calibration,
	&dev_attr_raw_data,
	&dev_attr_reactive_alert,
	NULL,
};

static int bma255_setup_pin(struct bma255_p *data)
{
	int ret;

	ret = gpio_request(data->acc_int1, "ACC_INT1");
	if (ret < 0) {
		pr_err("[SENSOR] %s - gpio %d request failed (%d)\n",
			__func__, data->acc_int1, ret);
		goto exit;
	}

	ret = gpio_direction_input(data->acc_int1);
	if (ret < 0) {
		pr_err("[SENSOR]: %s - failed to set gpio %d as input (%d)\n",
			__func__, data->acc_int1, ret);
		goto exit_acc_int1;
	}

	ret = gpio_request(data->acc_int2, "ACC_INT2");
	if (ret < 0) {
		pr_err("[SENSOR]: %s - gpio %d request failed (%d)\n",
			__func__, data->acc_int2, ret);
		goto exit_acc_int1;
	}

	ret = gpio_direction_input(data->acc_int2);
	if (ret < 0) {
		pr_err("[SENSOR]: %s - failed to set gpio %d as input (%d)\n",
			__func__, data->acc_int2, ret);
		goto exit_acc_int2;
	}

	goto exit;

exit_acc_int2:
	gpio_free(data->acc_int2);
exit_acc_int1:
	gpio_free(data->acc_int1);
exit:
	return ret;
}

static int bma255_input_init(struct bma255_p *data)
{
	int ret = 0;
	struct input_dev *dev;

	dev = input_allocate_device();
	if (!dev)
		return -ENOMEM;

	dev->name = MODULE_NAME;
	dev->id.bustype = BUS_I2C;

	input_set_capability(dev, EV_REL, REL_X);
	input_set_capability(dev, EV_REL, REL_Y);
	input_set_capability(dev, EV_REL, REL_Z);
	input_set_drvdata(dev, data);

	ret = input_register_device(dev);
	if (ret < 0) {
		input_free_device(dev);
		return ret;
	}

	ret = sensors_create_symlink(&dev->dev.kobj, dev->name);
	if (ret < 0) {
		input_unregister_device(dev);
		return ret;
	}

	/* sysfs node creation */
	ret = sysfs_create_group(&dev->dev.kobj, &bma255_attribute_group);
	if (ret < 0) {
		input_unregister_device(dev);
		return ret;
	}

	data->input = dev;
	return 0;
}

static int bma255_parse_dt(struct bma255_p *data, struct device *dev)
{
	struct device_node *dNode = dev->of_node;
	enum of_gpio_flags flags;

	if (dNode == NULL)
		return -ENODEV;

	data->acc_int1 = of_get_named_gpio_flags(dNode,
		"bma255-i2c,acc_int1-gpio", 0, &flags);
	if (data->acc_int1 < 0) {
		pr_err("[SENSOR]: %s - get acc_int1 error\n", __func__);
		return -ENODEV;
	}

	data->acc_int2 = of_get_named_gpio_flags(dNode,
		"bma255-i2c,acc_int2-gpio", 0, &flags);
	if (data->acc_int2 < 0) {
		pr_err("[SENSOR]: %s - acc_int2 error\n", __func__);
		return -ENODEV;
	}

	if (of_property_read_u32(dNode,
			"bma255-i2c,chip_pos", &data->chip_pos) < 0)
		data->chip_pos = BMA255_TOP_LOWER_RIGHT;

	return 0;
}

static int bma255_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int ret = -ENODEV;
	struct bma255_p *data = NULL;

	pr_info("##########################################################\n");
	pr_info("[SENSOR]: %s - Probe Start!\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("[SENSOR]: %s - i2c_check_functionality error\n",
			__func__);
		goto exit;
	}

	data = kzalloc(sizeof(struct bma255_p), GFP_KERNEL);
	if (data == NULL) {
		pr_err("[SENSOR]: %s - kzalloc error\n", __func__);
		ret = -ENOMEM;
		goto exit_kzalloc;
	}

	ret = bma255_parse_dt(data, &client->dev);
	if (ret < 0) {
		pr_err("[SENSOR]: %s - of_node error\n", __func__);
		ret = -ENODEV;
		goto exit_of_node;
	}

	ret = bma255_setup_pin(data);
	if (ret) {
		pr_err("[SENSOR]: %s - could not setup pin\n", __func__);
		goto exit_setup_pin;
	}

	/* read chip id */
	ret = i2c_smbus_read_word_data(client, BMA255_CHIP_ID_REG);
	if ((ret & 0x00ff) != BMA255_CHIP_ID) {
		pr_err("[SENSOR]: %s - chip id failed %d\n", __func__, ret);
		ret = -ENODEV;
		goto exit_read_chipid;
	}

	i2c_set_clientdata(client, data);
	data->client = client;

	/* input device init */
	ret = bma255_input_init(data);
	if (ret < 0)
		goto exit_input_init;

	sensors_register(data->factory_device, data, sensor_attrs, MODULE_NAME);

	/* workqueue init */
	INIT_DELAYED_WORK(&data->work, bma255_work_func);
	atomic_set(&data->delay, BMA255_DEFAULT_DELAY);
	atomic_set(&data->enable, 0);

	bma255_set_bandwidth(data, BMA255_BW_125HZ);
	bma255_set_range(data, BMA255_RANGE_2G);
	bma255_set_mode(data, BMA255_MODE_SUSPEND);

#ifdef EXECPTION_FOR_I2CFAIL
	data->i2cfail_cnt = 0;
#endif

	pr_info("[SENSOR]: %s - Probe done!(chip pos : %d)\n",
		__func__, data->chip_pos);

	return 0;

exit_input_init:
exit_read_chipid:
	gpio_free(data->acc_int2);
	gpio_free(data->acc_int1);
exit_setup_pin:
exit_of_node:
	kfree(data);
exit_kzalloc:
exit:
	pr_err("[SENSOR]: %s - Probe fail!\n", __func__);
	return ret;
}

static int __devexit bma255_remove(struct i2c_client *client)
{
	struct bma255_p *data = (struct bma255_p *)i2c_get_clientdata(client);

	if (atomic_read(&data->enable) == 1)
		bma255_set_enable(data, 0);

	cancel_delayed_work_sync(&data->work);
	sensors_unregister(data->factory_device, sensor_attrs);
	sensors_remove_symlink(&data->input->dev.kobj, data->input->name);

	sysfs_remove_group(&data->input->dev.kobj, &bma255_attribute_group);
	input_unregister_device(data->input);

	gpio_free(data->acc_int2);
	gpio_free(data->acc_int1);

	kfree(data);

	return 0;
}

static int bma255_suspend(struct device *dev)
{
	struct bma255_p *data = dev_get_drvdata(dev);

	if (atomic_read(&data->enable) == 1) {
		bma255_set_mode(data, BMA255_MODE_SUSPEND);
		cancel_delayed_work_sync(&data->work);
	}

	return 0;
}

static int bma255_resume(struct device *dev)
{
	struct bma255_p *data = dev_get_drvdata(dev);

	if (atomic_read(&data->enable) == 1) {
		bma255_set_mode(data, BMA255_MODE_NORMAL);
		schedule_delayed_work(&data->work,
			msecs_to_jiffies(atomic_read(&data->delay)));
	}

	return 0;
}

static struct of_device_id bma255_match_table[] = {
	{ .compatible = "bma255-i2c",},
	{},
};

static const struct i2c_device_id bma255_id[] = {
	{ "bma255_match_table", 0 },
	{ }
};

static const struct dev_pm_ops bma255_pm_ops = {
	.suspend = bma255_suspend,
	.resume = bma255_resume
};

static struct i2c_driver bma255_driver = {
	.driver = {
		.name	= MODEL_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = bma255_match_table,
		.pm = &bma255_pm_ops
	},
	.probe		= bma255_probe,
	.remove		= __devexit_p(bma255_remove),
	.id_table	= bma255_id,
};

static int __init BMA255_init(void)
{
	return i2c_add_driver(&bma255_driver);
}

static void __exit BMA255_exit(void)
{
	i2c_del_driver(&bma255_driver);
}

module_init(BMA255_init);
module_exit(BMA255_exit);

MODULE_DESCRIPTION("bma255 accelerometer sensor driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
