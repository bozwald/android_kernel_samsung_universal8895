/*
 *  max17048_fuelgauge.c
 *  Samsung MAX17048 Fuel Gauge Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/battery/sec_fuelgauge.h>
#include <linux/sec_batt.h>

#if 0
static int max17048_write_reg(struct i2c_client *client, int reg, u8 value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, reg, value);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}
#endif

static int max17048_read_reg(struct i2c_client *client, int reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

static int max17048_read_word(struct i2c_client *client, int reg)
{
	int ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

static int max17048_write_word(struct i2c_client *client, int reg, u16 buf)
{
	int ret;

	ret = i2c_smbus_write_word_data(client, reg, buf);
	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	return ret;
}

static void max17048_reset(struct i2c_client *client)
{
	u16 mode, reset_cmd;

	mode = max17048_read_word(client, MAX17048_MODE_MSB);

	mode = swab16(mode);
	reset_cmd = swab16(mode | 0x4000);

	i2c_smbus_write_word_data(client, MAX17048_MODE_MSB, reset_cmd);

	msleep(300);
}

static int max17048_get_vcell(struct i2c_client *client)
{
	u32 vcell;
	u16 w_data;
	u32 temp;

	temp = max17048_read_word(client, MAX17048_VCELL_MSB);

	w_data = swab16(temp);

	temp = ((w_data & 0xFFF0) >> 4) * 1250;
	vcell = temp / 1000;

	dev_dbg(&client->dev,
		"%s : vcell (%d)\n", __func__, vcell);

	return vcell;
}

static int max17048_get_avg_vcell(struct i2c_client *client)
{
	u32 vcell_data = 0;
	u32 vcell_max = 0;
	u32 vcell_min = 0;
	u32 vcell_total = 0;
	u32 i;

	for (i = 0; i < AVER_SAMPLE_CNT; i++) {
		vcell_data = max17048_get_vcell(client);

		if (i != 0) {
			if (vcell_data > vcell_max)
				vcell_max = vcell_data;
			else if (vcell_data < vcell_min)
				vcell_min = vcell_data;
		} else {
			vcell_max = vcell_data;
			vcell_min = vcell_data;
		}
		vcell_total += vcell_data;
	}

	return (vcell_total - vcell_max - vcell_min) / (AVER_SAMPLE_CNT-2);
}

static int max17048_get_ocv(struct i2c_client *client)
{
	u32 ocv;
	u16 w_data;
	u32 temp;
	u16 cmd;

	cmd = swab16(0x4A57);
	max17048_write_word(client, 0x3E, cmd);

	temp = max17048_read_word(client, MAX17048_OCV_MSB);

	w_data = swab16(temp);

	temp = ((w_data & 0xFFF0) >> 4) * 1250;
	ocv = temp / 1000;

	cmd = swab16(0x0000);
	max17048_write_word(client, 0x3E, cmd);

	dev_dbg(&client->dev,
		"%s : ocv (%d)\n", __func__, ocv);

	return ocv;
}

/* soc should be 0.01% unit */
static int max17048_get_soc(struct i2c_client *client)
{
	struct sec_fuelgauge_info *fuelgauge =
				i2c_get_clientdata(client);
	u8 data[2] = {0, 0};
	int temp, soc;
	u64 psoc64 = 0;
	u64 temp64;
	u32 divisor = 10000000;

	temp = max17048_read_word(client, MAX17048_SOC_MSB);

	if (get_battery_data(fuelgauge).is_using_model_data) {
		/* [ TempSOC = ((SOC1 * 256) + SOC2) * 0.001953125 ] */
		temp64 = swab16(temp);
		psoc64 = temp64 * 1953125;
		psoc64 = div_u64(psoc64, divisor);
		soc = psoc64 & 0xffff;
	} else {
		data[0] = temp & 0xff;
		data[1] = (temp & 0xff00) >> 8;

		soc = (data[0] * 100) + (data[1] * 100 / 256);
	}

	dev_dbg(&client->dev,
		"%s : raw capacity (%d), data(0x%04x)\n",
		__func__, soc, (data[0]<<8) | data[1]);

	return soc;
}

static int max17048_get_current(struct i2c_client *client)
{
	union power_supply_propval value;

	psy_do_property("sec-charger", get,
		POWER_SUPPLY_PROP_CURRENT_NOW, value);

	return value.intval;
}

#define DISCHARGE_SAMPLE_CNT 5
static int discharge_cnt=0;
static int all_vcell[5] = {0,};

/* if ret < 0, discharge */
static int sec_bat_check_discharge(int vcell)
{
	int i, cnt, ret = 0;

	all_vcell[discharge_cnt++] = vcell;
	if (discharge_cnt >= DISCHARGE_SAMPLE_CNT)
		discharge_cnt = 0;

	cnt = discharge_cnt;

	/* check after last value is set */
	if (all_vcell[cnt] == 0)
		return 0;

	for (i = 0; i < DISCHARGE_SAMPLE_CNT; i++) {
		if (cnt == i)
			continue;
		if (all_vcell[cnt] > all_vcell[i])
			ret--;
		else
			ret++;
	}
	return ret;
}

/* judge power off or not by current_avg */
static int max17048_get_current_average(struct i2c_client *client)
{
	union power_supply_propval value_bat;
	union power_supply_propval value_chg;
	int vcell, soc, curr_avg;
	int check_discharge;

	psy_do_property("sec-charger", get,
		POWER_SUPPLY_PROP_CURRENT_NOW, value_chg);
	psy_do_property("battery", get,
		POWER_SUPPLY_PROP_HEALTH, value_bat);
	vcell = max17048_get_vcell(client);
	soc = max17048_get_soc(client) / 100;
	check_discharge = sec_bat_check_discharge(vcell);

	/* if 0% && under 3.4v && low power charging(1000mA), power off */
	if (!lpcharge && (soc <= 0) && (vcell < 3400) &&
	    (check_discharge < 0) &&
	    (((value_bat.intval == POWER_SUPPLY_HEALTH_OVERHEAT) ||
	      (value_bat.intval == POWER_SUPPLY_HEALTH_COLD)))) {
		pr_info("%s: SOC(%d), Vnow(%d), Inow(%d)\n",
			__func__, soc, vcell, value_chg.intval);
		curr_avg = -1;
	} else {
		curr_avg = value_chg.intval;
	}

	return curr_avg;
}

void sec_bat_reset_discharge(struct i2c_client *client)
{
	int i;

	for (i = 0; i < DISCHARGE_SAMPLE_CNT ; i++)
		all_vcell[i] = 0;
	discharge_cnt = 0;
}

static void max17048_get_version(struct i2c_client *client)
{
	u16 w_data;
	int temp;

	temp = max17048_read_word(client, MAX17048_VER_MSB);

	w_data = swab16(temp);

	dev_info(&client->dev,
		"MAX17048 Fuel-Gauge Ver 0x%04x\n", w_data);
}

static u16 max17048_get_rcomp(struct i2c_client *client)
{
	u16 w_data;
	int temp;

	temp = max17048_read_word(client, MAX17048_RCOMP_MSB);

	w_data = swab16(temp);

	dev_dbg(&client->dev,
		"%s : current rcomp = 0x%04x\n",
		__func__, w_data);

	return w_data;
}

static void max17048_set_rcomp(struct i2c_client *client, u16 new_rcomp)
{
	i2c_smbus_write_word_data(client,
		MAX17048_RCOMP_MSB, swab16(new_rcomp));
}

static void max17048_rcomp_update(struct i2c_client *client, int temp)
{
	struct sec_fuelgauge_info *fuelgauge =
				i2c_get_clientdata(client);
	union power_supply_propval value;

	int starting_rcomp = 0;
	int new_rcomp = 0;
	int rcomp_current = 0;

	rcomp_current = max17048_get_rcomp(client);

	psy_do_property("battery", get,
		POWER_SUPPLY_PROP_STATUS, value);

	if (value.intval == POWER_SUPPLY_STATUS_CHARGING) /* in charging */
		starting_rcomp = get_battery_data(fuelgauge).RCOMP_charging;
	else
		starting_rcomp = get_battery_data(fuelgauge).RCOMP0;

	if (temp > RCOMP0_TEMP)
		new_rcomp = starting_rcomp + ((temp - RCOMP0_TEMP) *
			get_battery_data(fuelgauge).temp_cohot / 1000);
	else if (temp < RCOMP0_TEMP)
		new_rcomp = starting_rcomp + ((temp - RCOMP0_TEMP) *
			get_battery_data(fuelgauge).temp_cocold / 1000);
	else
		new_rcomp = starting_rcomp;

	if (new_rcomp > 255)
		new_rcomp = 255;
	else if (new_rcomp < 0)
		new_rcomp = 0;

	new_rcomp <<= 8;
	new_rcomp &= 0xff00;
	/* not related to RCOMP */
	new_rcomp |= (rcomp_current & 0xff);

	if (rcomp_current != new_rcomp) {
		dev_dbg(&client->dev,
			"%s : RCOMP 0x%04x -> 0x%04x (0x%02x)\n",
			__func__, rcomp_current, new_rcomp,
			new_rcomp >> 8);
		max17048_set_rcomp(client, new_rcomp);
	}
}

#ifdef CONFIG_OF
static int max17048_parse_dt(struct device *dev,
			     struct sec_fuelgauge_info *fuelgauge)
{
	struct device_node *np = dev->of_node;
	int ret;
	int value;

	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_u32(np, "fuelgauge,rcomp0",
					   &value);
		pr_err("%s value %d\n",
		       __func__, value);
		get_battery_data(fuelgauge).RCOMP0 = (u8)value;
		if (ret < 0)
			pr_err("%s error reading rcomp0 %d\n",
			       __func__, ret);
		ret = of_property_read_u32(np, "fuelgauge,rcomp_charging",
					   &value);
		pr_err("%s value %d\n",
		       __func__, value);
		get_battery_data(fuelgauge).RCOMP_charging = (u8)value;
		if (ret < 0)
			pr_err("%s error reading rcomp_charging %d\n",
			       __func__, ret);
		ret = of_property_read_u32(np, "fuelgauge,temp_cohot",
				   &get_battery_data(fuelgauge).temp_cohot);
		if (ret < 0)
			pr_err("%s error reading temp_cohot %d\n",
			       __func__, ret);
		ret = of_property_read_u32(np, "fuelgauge,temp_cocold",
				   &get_battery_data(fuelgauge).temp_cocold);
		if (ret < 0)
			pr_err("%s error reading temp_cocold %d\n",
			       __func__, ret);
		get_battery_data(fuelgauge).is_using_model_data = of_property_read_bool(np,
				"fuelgauge,is_using_model_data");
		ret = of_property_read_string(np, "fuelgauge,type_str",
				(const char **)&get_battery_data(fuelgauge).type_str);
		if (ret < 0)
			pr_err("%s error reading temp_cocold %d\n",
			       __func__, ret);

		pr_info("%s RCOMP0: 0x%x, RCOMP_charging: 0x%x, temp_cohot: %d,"
			"temp_cocold: %d, is_using_model_data: %d, "
			"type_str: %s,\n", __func__,
			get_battery_data(fuelgauge).RCOMP0,
			get_battery_data(fuelgauge).RCOMP_charging,
			get_battery_data(fuelgauge).temp_cohot,
			get_battery_data(fuelgauge).temp_cocold,
			get_battery_data(fuelgauge).is_using_model_data,
			get_battery_data(fuelgauge).type_str
			);
	}

	return 0;
}
#endif

static void fg_read_regs(struct i2c_client *client, char *str)
{
	int data = 0;
	u32 addr = 0;

	for (addr = 0x02; addr <= 0x04; addr += 2) {
		data = max17048_read_word(client, addr);
		sprintf(str + strlen(str), "0x%04x, ", data);
	}

	/* "#" considered as new line in application */
	sprintf(str+strlen(str), "#");

	for (addr = 0x08; addr <= 0x1a; addr += 2) {
		data = max17048_read_word(client, addr);
		sprintf(str + strlen(str), "0x%04x, ", data);
	}
}

bool sec_hal_fg_init(struct i2c_client *client)
{
#ifdef CONFIG_OF
	struct sec_fuelgauge_info *fuelgauge =
		i2c_get_clientdata(client);
	int error;

	error = max17048_parse_dt(&client->dev, fuelgauge);

	if (error) {
		dev_err(&client->dev,
			"%s : Failed to get max17048 fuel_init\n", __func__);
		return false;
	}
#endif
	pr_info("%s\n", __func__);

	max17048_get_version(client);

	return true;
}

bool sec_hal_fg_suspend(struct i2c_client *client)
{
	return true;
}

bool sec_hal_fg_resume(struct i2c_client *client)
{
	return true;
}

bool sec_hal_fg_fuelalert_init(struct i2c_client *client, int soc)
{
	u16 temp;
	u8 data;

	temp = max17048_get_rcomp(client);
	data = 32 - soc; /* set soc for fuel alert */
	temp &= 0xff00;
	temp += data;

	dev_dbg(&client->dev,
		"%s : new rcomp = 0x%04x\n",
		__func__, temp);

	max17048_set_rcomp(client, temp);

	return true;
}

bool sec_hal_fg_is_fuelalerted(struct i2c_client *client)
{
	u16 temp;

	temp = max17048_get_rcomp(client);

	if (temp & 0x20)	/* ALRT is asserted */
		return true;

	return false;
}

bool sec_hal_fg_fuelalert_process(void *irq_data, bool is_fuel_alerted)
{
	return true;
}

bool sec_hal_fg_full_charged(struct i2c_client *client)
{
	return true;
}

bool sec_hal_fg_reset(struct i2c_client *client)
{
	max17048_reset(client);
	return true;
}

bool sec_hal_fg_get_property(struct i2c_client *client,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	int i, pr_cnt = 1;
	union power_supply_propval value_bat;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = 0;
		break;
		/* Cell voltage (VCELL, mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max17048_get_vcell(client);
		break;
		/* Additional Voltage Information (mV) */
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		switch (val->intval) {
		case SEC_BATTEY_VOLTAGE_AVERAGE:
			val->intval = max17048_get_avg_vcell(client);
			break;
		case SEC_BATTEY_VOLTAGE_OCV:
			val->intval = max17048_get_ocv(client);
			break;
		}
		break;
		/* Current (mA) */
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		psy_do_property("battery", get,
			POWER_SUPPLY_PROP_STATUS, value_bat);
		if(value_bat.intval == POWER_SUPPLY_STATUS_DISCHARGING)
			val->intval = -max17048_get_current(client);
		else
			val->intval = max17048_get_current(client);
		break;
		/* Average Current (mA) */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = max17048_get_current_average(client);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		break;
		/* SOC (%) */
	case POWER_SUPPLY_PROP_CAPACITY:
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RAW) {
                        val->intval = max17048_get_soc(client);
                } else {
                        val->intval = max17048_get_soc(client) / 10;
			if (!(pr_cnt++ % 10)) {
				pr_cnt = 1;
				for (i = 0x02; i < 0x1C; i++)
					printk("0x%02x(0x%02x), ",
					i, max17048_read_reg(client, i));
				printk("\n");
			}
                }
		break;
		/* Battery Temperature */
	case POWER_SUPPLY_PROP_TEMP:
		/* Target Temperature */
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		break;
	default:
		return false;
	}
	return true;
}

bool sec_hal_fg_set_property(struct i2c_client *client,
			     enum power_supply_property psp,
			     const union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		sec_bat_reset_discharge(client);
		break;
		/* Battery Temperature */
	case POWER_SUPPLY_PROP_TEMP:
		/* Target Temperature */
		/* temperature is 0.1 degree, should be divide by 10 */
		max17048_rcomp_update(client, val->intval / 10);
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		break;
	default:
		return false;
	}
	return true;
}

ssize_t sec_hal_fg_show_attrs(struct device *dev,
				const ptrdiff_t offset, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sec_fuelgauge_info *fg =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);
	int i = 0;
	char *str = NULL;

	switch (offset) {
	case FG_DATA:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%02x%02x\n",
			fg->reg_data[1], fg->reg_data[0]);
		break;
	case FG_REGS:
		str = kzalloc(sizeof(char)*1024, GFP_KERNEL);
		if (!str)
			return -ENOMEM;

		fg_read_regs(fg->client, str);
		i += scnprintf(buf + i, PAGE_SIZE - i, "%s\n",
			str);

		kfree(str);
		break;
	default:
		i = -EINVAL;
		break;
	}

	return i;
}

ssize_t sec_hal_fg_store_attrs(struct device *dev,
				const ptrdiff_t offset,
				const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sec_fuelgauge_info *fg =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);
	int ret = 0;
	int x = 0;
	u16 data;

	switch (offset) {
	case FG_REG:
		if (sscanf(buf, "%x\n", &x) == 1) {
			fg->reg_addr = x;
			data = max17048_read_word(
				fg->client, fg->reg_addr);
			fg->reg_data[0] = (data & 0xff00) >> 8;
			fg->reg_data[1] = (data & 0x00ff);

			dev_dbg(&fg->client->dev,
				"%s: (read) addr = 0x%x, data = 0x%02x%02x\n",
				 __func__, fg->reg_addr,
				 fg->reg_data[1], fg->reg_data[0]);
			ret = count;
		}
		break;
	case FG_DATA:
		if (sscanf(buf, "%x\n", &x) == 1) {
			dev_dbg(&fg->client->dev,
				"%s: (write) addr = 0x%x, data = 0x%04x\n",
				__func__, fg->reg_addr, x);
			i2c_smbus_write_word_data(fg->client,
				fg->reg_addr, swab16(x));
			ret = count;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
