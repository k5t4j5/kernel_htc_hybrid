/*
 * Device driver for monitoring ambient light intensity (lux)
 * within the TAOS tsl258x family of devices (tsl2580, tsl2581).
 *
 * Copyright (c) 2011, TAOS Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA	02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/module.h>
#include "../iio.h"

#define TSL258X_MAX_DEVICE_REGS		32

#define	TSL258X_REG_MAX		8

#define TSL258X_CNTRL			0x00
#define TSL258X_ALS_TIME		0X01
#define TSL258X_INTERRUPT		0x02
#define TSL258X_GAIN			0x07
#define TSL258X_REVID			0x11
#define TSL258X_CHIPID			0x12
#define TSL258X_ALS_CHAN0LO		0x14
#define TSL258X_ALS_CHAN0HI		0x15
#define TSL258X_ALS_CHAN1LO		0x16
#define TSL258X_ALS_CHAN1HI		0x17
#define TSL258X_TMR_LO			0x18
#define TSL258X_TMR_HI			0x19

#define TSL258X_CMD_REG			0x80
#define TSL258X_CMD_SPL_FN		0x60
#define TSL258X_CMD_ALS_INT_CLR	0X01

#define TSL258X_CNTL_ADC_ENBL	0x02
#define TSL258X_CNTL_PWR_ON		0x01

#define TSL258X_STA_ADC_VALID	0x01
#define TSL258X_STA_ADC_INTR	0x10

#define	TSL258X_LUX_CALC_OVER_FLOW		65535

enum {
	TSL258X_CHIP_UNKNOWN = 0,
	TSL258X_CHIP_WORKING = 1,
	TSL258X_CHIP_SUSPENDED = 2
};

struct taos_als_info {
	u16 als_ch0;
	u16 als_ch1;
	u16 lux;
};

struct taos_settings {
	int als_time;
	int als_gain;
	int als_gain_trim;
	int als_cal_target;
};

struct tsl2583_chip {
	struct mutex als_mutex;
	struct i2c_client *client;
	struct taos_als_info als_cur_info;
	struct taos_settings taos_settings;
	int als_time_scale;
	int als_saturation;
	int taos_chip_status;
	u8 taos_config[8];
};

static const u8 taos_config[8] = {
		0x00, 0xee, 0x00, 0x03, 0x00, 0xFF, 0xFF, 0x00
}; 

struct taos_lux {
	unsigned int ratio;
	unsigned int ch0;
	unsigned int ch1;
};

static struct taos_lux taos_device_lux[11] = {
	{  9830,  8520, 15729 },
	{ 12452, 10807, 23344 },
	{ 14746,  6383, 11705 },
	{ 17695,  4063,  6554 },
};

struct gainadj {
	s16 ch0;
	s16 ch1;
};

static const struct gainadj gainadj[] = {
	{ 1, 1 },
	{ 8, 8 },
	{ 16, 16 },
	{ 107, 115 }
};

static void taos_defaults(struct tsl2583_chip *chip)
{
	
	chip->taos_settings.als_time = 100;
	
	chip->taos_settings.als_gain = 0;
	
	
	chip->taos_settings.als_gain_trim = 1000;
	
	chip->taos_settings.als_cal_target = 130;
	
}

static int
taos_i2c_read(struct i2c_client *client, u8 reg, u8 *val, unsigned int len)
{
	int i, ret;

	for (i = 0; i < len; i++) {
		
		ret = i2c_smbus_write_byte(client, (TSL258X_CMD_REG | reg));
		if (ret < 0) {
			dev_err(&client->dev, "taos_i2c_read failed to write"
				" register %x\n", reg);
			return ret;
		}
		
		*val = i2c_smbus_read_byte(client);
		val++;
		reg++;
	}
	return 0;
}

static int taos_get_lux(struct iio_dev *indio_dev)
{
	u16 ch0, ch1; 
	u32 lux; 
	u64 lux64;
	u32 ratio;
	u8 buf[5];
	struct taos_lux *p;
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	int i, ret;
	u32 ch0lux = 0;
	u32 ch1lux = 0;

	if (mutex_trylock(&chip->als_mutex) == 0) {
		dev_info(&chip->client->dev, "taos_get_lux device is busy\n");
		return chip->als_cur_info.lux; 
	}

	if (chip->taos_chip_status != TSL258X_CHIP_WORKING) {
		
		dev_err(&chip->client->dev, "taos_get_lux device is not enabled\n");
		ret = -EBUSY ;
		goto out_unlock;
	}

	ret = taos_i2c_read(chip->client, (TSL258X_CMD_REG), &buf[0], 1);
	if (ret < 0) {
		dev_err(&chip->client->dev, "taos_get_lux failed to read CMD_REG\n");
		goto out_unlock;
	}
	
	if (!(buf[0] & TSL258X_STA_ADC_INTR)) {
		dev_err(&chip->client->dev, "taos_get_lux data not valid\n");
		ret = chip->als_cur_info.lux; 
		goto out_unlock;
	}

	for (i = 0; i < 4; i++) {
		int reg = TSL258X_CMD_REG | (TSL258X_ALS_CHAN0LO + i);
		ret = taos_i2c_read(chip->client, reg, &buf[i], 1);
		if (ret < 0) {
			dev_err(&chip->client->dev, "taos_get_lux failed to read"
				" register %x\n", reg);
			goto out_unlock;
		}
	}

	ret = i2c_smbus_write_byte(chip->client,
				   (TSL258X_CMD_REG | TSL258X_CMD_SPL_FN |
				    TSL258X_CMD_ALS_INT_CLR));

	if (ret < 0) {
		dev_err(&chip->client->dev,
			"taos_i2c_write_command failed in taos_get_lux, err = %d\n",
			ret);
		goto out_unlock; 
	}

	
	ch0 = le16_to_cpup((const __le16 *)&buf[0]);
	ch1 = le16_to_cpup((const __le16 *)&buf[2]);

	chip->als_cur_info.als_ch0 = ch0;
	chip->als_cur_info.als_ch1 = ch1;

	if ((ch0 >= chip->als_saturation) || (ch1 >= chip->als_saturation))
		goto return_max;

	if (ch0 == 0) {
		
		ret = chip->als_cur_info.lux = 0;
		goto out_unlock;
	}
	
	ratio = (ch1 << 15) / ch0;
	
	for (p = (struct taos_lux *) taos_device_lux;
	     p->ratio != 0 && p->ratio < ratio; p++)
		;

	if (p->ratio == 0) {
		lux = 0;
	} else {
		ch0lux = ((ch0 * p->ch0) +
			  (gainadj[chip->taos_settings.als_gain].ch0 >> 1))
			 / gainadj[chip->taos_settings.als_gain].ch0;
		ch1lux = ((ch1 * p->ch1) +
			  (gainadj[chip->taos_settings.als_gain].ch1 >> 1))
			 / gainadj[chip->taos_settings.als_gain].ch1;
		lux = ch0lux - ch1lux;
	}

	
	if (ch1lux > ch0lux) {
		dev_dbg(&chip->client->dev, "No Data - Return last value\n");
		ret = chip->als_cur_info.lux = 0;
		goto out_unlock;
	}

	
	if (chip->als_time_scale == 0)
		lux = 0;
	else
		lux = (lux + (chip->als_time_scale >> 1)) /
			chip->als_time_scale;

	lux64 = lux;
	lux64 = lux64 * chip->taos_settings.als_gain_trim;
	lux64 >>= 13;
	lux = lux64;
	lux = (lux + 500) / 1000;
	if (lux > TSL258X_LUX_CALC_OVER_FLOW) { 
return_max:
		lux = TSL258X_LUX_CALC_OVER_FLOW;
	}

	
	chip->als_cur_info.lux = lux;
	ret = lux;

out_unlock:
	mutex_unlock(&chip->als_mutex);
	return ret;
}

static int taos_als_calibrate(struct iio_dev *indio_dev)
{
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	u8 reg_val;
	unsigned int gain_trim_val;
	int ret;
	int lux_val;

	ret = i2c_smbus_write_byte(chip->client,
				   (TSL258X_CMD_REG | TSL258X_CNTRL));
	if (ret < 0) {
		dev_err(&chip->client->dev,
			"taos_als_calibrate failed to reach the CNTRL register, ret=%d\n",
			ret);
		return ret;
	}

	reg_val = i2c_smbus_read_byte(chip->client);
	if ((reg_val & (TSL258X_CNTL_ADC_ENBL | TSL258X_CNTL_PWR_ON))
			!= (TSL258X_CNTL_ADC_ENBL | TSL258X_CNTL_PWR_ON)) {
		dev_err(&chip->client->dev,
			"taos_als_calibrate failed: device not powered on with ADC enabled\n");
		return -1;
	}

	ret = i2c_smbus_write_byte(chip->client,
				   (TSL258X_CMD_REG | TSL258X_CNTRL));
	if (ret < 0) {
		dev_err(&chip->client->dev,
			"taos_als_calibrate failed to reach the STATUS register, ret=%d\n",
			ret);
		return ret;
	}
	reg_val = i2c_smbus_read_byte(chip->client);

	if ((reg_val & TSL258X_STA_ADC_VALID) != TSL258X_STA_ADC_VALID) {
		dev_err(&chip->client->dev,
			"taos_als_calibrate failed: STATUS - ADC not valid.\n");
		return -ENODATA;
	}
	lux_val = taos_get_lux(indio_dev);
	if (lux_val < 0) {
		dev_err(&chip->client->dev, "taos_als_calibrate failed to get lux\n");
		return lux_val;
	}
	gain_trim_val = (unsigned int) (((chip->taos_settings.als_cal_target)
			* chip->taos_settings.als_gain_trim) / lux_val);

	if ((gain_trim_val < 250) || (gain_trim_val > 4000)) {
		dev_err(&chip->client->dev,
			"taos_als_calibrate failed: trim_val of %d is out of range\n",
			gain_trim_val);
		return -ENODATA;
	}
	chip->taos_settings.als_gain_trim = (int) gain_trim_val;

	return (int) gain_trim_val;
}

static int taos_chip_on(struct iio_dev *indio_dev)
{
	int i;
	int ret;
	u8 *uP;
	u8 utmp;
	int als_count;
	int als_time;
	struct tsl2583_chip *chip = iio_priv(indio_dev);

	
	if (chip->taos_chip_status == TSL258X_CHIP_WORKING) {
		
		dev_info(&chip->client->dev, "device is already enabled\n");
		return -EINVAL;
	}

	
	als_count = (chip->taos_settings.als_time * 100 + 135) / 270;
	if (als_count == 0)
		als_count = 1; 

	
	als_time = (als_count * 27 + 5) / 10;
	chip->taos_config[TSL258X_ALS_TIME] = 256 - als_count;

	
	chip->taos_config[TSL258X_GAIN] = chip->taos_settings.als_gain;

	
	chip->als_saturation = als_count * 922; 
	chip->als_time_scale = (als_time + 25) / 50;

	utmp = TSL258X_CNTL_PWR_ON;
	ret = i2c_smbus_write_byte_data(chip->client,
					TSL258X_CMD_REG | TSL258X_CNTRL, utmp);
	if (ret < 0) {
		dev_err(&chip->client->dev, "taos_chip_on failed on CNTRL reg.\n");
		return -1;
	}

	for (i = 0, uP = chip->taos_config; i < TSL258X_REG_MAX; i++) {
		ret = i2c_smbus_write_byte_data(chip->client,
						TSL258X_CMD_REG + i,
						*uP++);
		if (ret < 0) {
			dev_err(&chip->client->dev,
				"taos_chip_on failed on reg %d.\n", i);
			return -1;
		}
	}

	msleep(3);
	utmp = TSL258X_CNTL_PWR_ON | TSL258X_CNTL_ADC_ENBL;
	ret = i2c_smbus_write_byte_data(chip->client,
					TSL258X_CMD_REG | TSL258X_CNTRL,
					utmp);
	if (ret < 0) {
		dev_err(&chip->client->dev, "taos_chip_on failed on 2nd CTRL reg.\n");
		return -1;
	}
	chip->taos_chip_status = TSL258X_CHIP_WORKING;

	return ret;
}

static int taos_chip_off(struct iio_dev *indio_dev)
{
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	int ret;

	
	chip->taos_chip_status = TSL258X_CHIP_SUSPENDED;
	ret = i2c_smbus_write_byte_data(chip->client,
					TSL258X_CMD_REG | TSL258X_CNTRL,
					0x00);
	return ret;
}


static ssize_t taos_power_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", chip->taos_chip_status);
}

static ssize_t taos_power_state_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	if (value == 0)
		taos_chip_off(indio_dev);
	else
		taos_chip_on(indio_dev);

	return len;
}

static ssize_t taos_gain_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	char gain[4] = {0};

	switch (chip->taos_settings.als_gain) {
	case 0:
		strcpy(gain, "001");
		break;
	case 1:
		strcpy(gain, "008");
		break;
	case 2:
		strcpy(gain, "016");
		break;
	case 3:
		strcpy(gain, "111");
		break;
	}

	return sprintf(buf, "%s\n", gain);
}

static ssize_t taos_gain_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	switch (value) {
	case 1:
		chip->taos_settings.als_gain = 0;
		break;
	case 8:
		chip->taos_settings.als_gain = 1;
		break;
	case 16:
		chip->taos_settings.als_gain = 2;
		break;
	case 111:
		chip->taos_settings.als_gain = 3;
		break;
	default:
		dev_err(dev, "Invalid Gain Index (must be 1,8,16,111)\n");
		return -1;
	}

	return len;
}

static ssize_t taos_gain_available_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", "1 8 16 111");
}

static ssize_t taos_als_time_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", chip->taos_settings.als_time);
}

static ssize_t taos_als_time_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	if ((value < 50) || (value > 650))
		return -EINVAL;

	if (value % 50)
		return -EINVAL;

	chip->taos_settings.als_time = value;

	return len;
}

static ssize_t taos_als_time_available_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n",
		"50 100 150 200 250 300 350 400 450 500 550 600 650");
}

static ssize_t taos_als_trim_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", chip->taos_settings.als_gain_trim);
}

static ssize_t taos_als_trim_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	if (value)
		chip->taos_settings.als_gain_trim = value;

	return len;
}

static ssize_t taos_als_cal_target_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", chip->taos_settings.als_cal_target);
}

static ssize_t taos_als_cal_target_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	if (value)
		chip->taos_settings.als_cal_target = value;

	return len;
}

static ssize_t taos_lux_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	int ret;

	ret = taos_get_lux(dev_get_drvdata(dev));
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", ret);
}

static ssize_t taos_do_calibrate(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	unsigned long value;

	if (strict_strtoul(buf, 0, &value))
		return -EINVAL;

	if (value == 1)
		taos_als_calibrate(indio_dev);

	return len;
}

static ssize_t taos_luxtable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int i;
	int offset = 0;

	for (i = 0; i < ARRAY_SIZE(taos_device_lux); i++) {
		offset += sprintf(buf + offset, "%d,%d,%d,",
				  taos_device_lux[i].ratio,
				  taos_device_lux[i].ch0,
				  taos_device_lux[i].ch1);
		if (taos_device_lux[i].ratio == 0) {
			offset--;
			break;
		}
	}

	offset += sprintf(buf + offset, "\n");
	return offset;
}

static ssize_t taos_luxtable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	int value[ARRAY_SIZE(taos_device_lux)*3 + 1];
	int n;

	get_options(buf, ARRAY_SIZE(value), value);

	n = value[0];
	if ((n % 3) || n < 6 || n > ((ARRAY_SIZE(taos_device_lux) - 1) * 3)) {
		dev_info(dev, "LUX TABLE INPUT ERROR 1 Value[0]=%d\n", n);
		return -EINVAL;
	}
	if ((value[(n - 2)] | value[(n - 1)] | value[n]) != 0) {
		dev_info(dev, "LUX TABLE INPUT ERROR 2 Value[0]=%d\n", n);
		return -EINVAL;
	}

	if (chip->taos_chip_status == TSL258X_CHIP_WORKING)
		taos_chip_off(indio_dev);

	
	memset(taos_device_lux, 0, sizeof(taos_device_lux));
	memcpy(taos_device_lux, &value[1], (value[0] * 4));

	taos_chip_on(indio_dev);

	return len;
}

static DEVICE_ATTR(power_state, S_IRUGO | S_IWUSR,
		taos_power_state_show, taos_power_state_store);

static DEVICE_ATTR(illuminance0_calibscale, S_IRUGO | S_IWUSR,
		taos_gain_show, taos_gain_store);
static DEVICE_ATTR(illuminance0_calibscale_available, S_IRUGO,
		taos_gain_available_show, NULL);

static DEVICE_ATTR(illuminance0_integration_time, S_IRUGO | S_IWUSR,
		taos_als_time_show, taos_als_time_store);
static DEVICE_ATTR(illuminance0_integration_time_available, S_IRUGO,
		taos_als_time_available_show, NULL);

static DEVICE_ATTR(illuminance0_calibbias, S_IRUGO | S_IWUSR,
		taos_als_trim_show, taos_als_trim_store);

static DEVICE_ATTR(illuminance0_input_target, S_IRUGO | S_IWUSR,
		taos_als_cal_target_show, taos_als_cal_target_store);

static DEVICE_ATTR(illuminance0_input, S_IRUGO, taos_lux_show, NULL);
static DEVICE_ATTR(illuminance0_calibrate, S_IWUSR, NULL, taos_do_calibrate);
static DEVICE_ATTR(illuminance0_lux_table, S_IRUGO | S_IWUSR,
		taos_luxtable_show, taos_luxtable_store);

static struct attribute *sysfs_attrs_ctrl[] = {
	&dev_attr_power_state.attr,
	&dev_attr_illuminance0_calibscale.attr,			
	&dev_attr_illuminance0_calibscale_available.attr,
	&dev_attr_illuminance0_integration_time.attr,	
	&dev_attr_illuminance0_integration_time_available.attr,
	&dev_attr_illuminance0_calibbias.attr,			
	&dev_attr_illuminance0_input_target.attr,
	&dev_attr_illuminance0_input.attr,
	&dev_attr_illuminance0_calibrate.attr,
	&dev_attr_illuminance0_lux_table.attr,
	NULL
};

static struct attribute_group tsl2583_attribute_group = {
	.attrs = sysfs_attrs_ctrl,
};

static int taos_tsl258x_device(unsigned char *bufp)
{
	return ((bufp[TSL258X_CHIPID] & 0xf0) == 0x90);
}

static const struct iio_info tsl2583_info = {
	.attrs = &tsl2583_attribute_group,
	.driver_module = THIS_MODULE,
};

static int __devinit taos_probe(struct i2c_client *clientp,
		      const struct i2c_device_id *idp)
{
	int i, ret;
	unsigned char buf[TSL258X_MAX_DEVICE_REGS];
	struct tsl2583_chip *chip;
	struct iio_dev *indio_dev;

	if (!i2c_check_functionality(clientp->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&clientp->dev,
			"taos_probe() - i2c smbus byte data "
			"functions unsupported\n");
		return -EOPNOTSUPP;
	}

	indio_dev = iio_allocate_device(sizeof(*chip));
	if (indio_dev == NULL) {
		ret = -ENOMEM;
		dev_err(&clientp->dev, "iio allocation failed\n");
		goto fail1;
	}
	chip = iio_priv(indio_dev);
	chip->client = clientp;
	i2c_set_clientdata(clientp, indio_dev);

	mutex_init(&chip->als_mutex);
	chip->taos_chip_status = TSL258X_CHIP_UNKNOWN;
	memcpy(chip->taos_config, taos_config, sizeof(chip->taos_config));

	for (i = 0; i < TSL258X_MAX_DEVICE_REGS; i++) {
		ret = i2c_smbus_write_byte(clientp,
				(TSL258X_CMD_REG | (TSL258X_CNTRL + i)));
		if (ret < 0) {
			dev_err(&clientp->dev, "i2c_smbus_write_bytes() to cmd "
				"reg failed in taos_probe(), err = %d\n", ret);
			goto fail2;
		}
		ret = i2c_smbus_read_byte(clientp);
		if (ret < 0) {
			dev_err(&clientp->dev, "i2c_smbus_read_byte from "
				"reg failed in taos_probe(), err = %d\n", ret);

			goto fail2;
		}
		buf[i] = ret;
	}

	if (!taos_tsl258x_device(buf)) {
		dev_info(&clientp->dev, "i2c device found but does not match "
			"expected id in taos_probe()\n");
		goto fail2;
	}

	ret = i2c_smbus_write_byte(clientp, (TSL258X_CMD_REG | TSL258X_CNTRL));
	if (ret < 0) {
		dev_err(&clientp->dev, "i2c_smbus_write_byte() to cmd reg "
			"failed in taos_probe(), err = %d\n", ret);
		goto fail2;
	}

	indio_dev->info = &tsl2583_info;
	indio_dev->dev.parent = &clientp->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = chip->client->name;
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&clientp->dev, "iio registration failed\n");
		goto fail2;
	}

	
	taos_defaults(chip);

	
	taos_chip_on(indio_dev);

	dev_info(&clientp->dev, "Light sensor found.\n");
	return 0;
fail1:
	iio_free_device(indio_dev);
fail2:
	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int taos_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&chip->als_mutex);

	if (chip->taos_chip_status == TSL258X_CHIP_WORKING) {
		ret = taos_chip_off(indio_dev);
		chip->taos_chip_status = TSL258X_CHIP_SUSPENDED;
	}

	mutex_unlock(&chip->als_mutex);
	return ret;
}

static int taos_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct tsl2583_chip *chip = iio_priv(indio_dev);
	int ret = 0;

	mutex_lock(&chip->als_mutex);

	if (chip->taos_chip_status == TSL258X_CHIP_SUSPENDED)
		ret = taos_chip_on(indio_dev);

	mutex_unlock(&chip->als_mutex);
	return ret;
}

static SIMPLE_DEV_PM_OPS(taos_pm_ops, taos_suspend, taos_resume);
#define TAOS_PM_OPS (&taos_pm_ops)
#else
#define TAOS_PM_OPS NULL
#endif

static int __devexit taos_remove(struct i2c_client *client)
{
	iio_device_unregister(i2c_get_clientdata(client));
	iio_free_device(i2c_get_clientdata(client));

	return 0;
}

static struct i2c_device_id taos_idtable[] = {
	{ "tsl2580", 0 },
	{ "tsl2581", 1 },
	{ "tsl2583", 2 },
	{}
};
MODULE_DEVICE_TABLE(i2c, taos_idtable);

static struct i2c_driver taos_driver = {
	.driver = {
		.name = "tsl2583",
		.pm = TAOS_PM_OPS,
	},
	.id_table = taos_idtable,
	.probe = taos_probe,
	.remove = __devexit_p(taos_remove),
};
module_i2c_driver(taos_driver);

MODULE_AUTHOR("J. August Brenner<jbrenner@taosinc.com>");
MODULE_DESCRIPTION("TAOS tsl2583 ambient light sensor driver");
MODULE_LICENSE("GPL");
