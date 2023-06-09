/*
 * SGM41511 battery charging driver
 *
 * Copyright (C) 2021 SGM
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#define pr_fmt(fmt)	"[sgm41511]:%s: " fmt, __func__

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/math64.h>

#include <charger_class.h>
#include <mtk_charger.h>

#include "sgm41511_reg.h"
#include "sgm41511.h"

enum {
	PN_SGM41511,
};

enum sgm41511_part_no {
	SGM41511 = 0x02,
};

/* add to distinguish sgm or bq */
enum extra_part_no {
	EXTRA_BQ25601 = 0x00,
	EXTRA_SGM41511 = 0x01,
};

/*HS03s for DEVAL5625-1795 by wenyaqi at 20210624 start*/
/* add to distinguish eta or sgm */
enum reg0c_no {
	REG0C_ETA6953 = 0x00,
};
/*HS03s for DEVAL5625-1795 by wenyaqi at 20210624 end*/

static int pn_data[] = {
	[PN_SGM41511] = 0x02,
};

static char *pn_str[] = {
	[PN_SGM41511] = "sgm41511",
};

struct sgm41511 {
	struct device *dev;
	struct i2c_client *client;

	enum sgm41511_part_no part_no;
	enum extra_part_no e_part_no; /* add to distinguish sgm or bq */
	enum reg0c_no reg0c_no; /* add to distinguish eta or sgm */
	int revision;

	const char *chg_dev_name;
	const char *eint_name;

	bool chg_det_enable;

	// enum charger_type chg_type;
	struct power_supply_desc psy_desc;
	int psy_usb_type;

	int status;
	int irq;

	struct mutex i2c_rw_lock;

	bool charge_enabled;	/* Register bit status */
	bool power_good;

	struct sgm41511_platform_data *platform_data;
	struct charger_device *chg_dev;

	struct power_supply *psy;
};

static const struct charger_properties sgm41511_chg_props = {
	.alias_name = "sgm41511",
};

static int __sgm41511_read_reg(struct sgm41511 *sgm, u8 reg, u8 *data)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(sgm->client, reg);
	if (ret < 0) {
		pr_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u8) ret;

	return 0;
}

static int __sgm41511_write_reg(struct sgm41511 *sgm, int reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(sgm->client, reg, val);
	if (ret < 0) {
		pr_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
				val, reg, ret);
		return ret;
	}
	return 0;
}

static int sgm41511_read_byte(struct sgm41511 *sgm, u8 reg, u8 *data)
{
	int ret;

	mutex_lock(&sgm->i2c_rw_lock);
	ret = __sgm41511_read_reg(sgm, reg, data);
	mutex_unlock(&sgm->i2c_rw_lock);

	return ret;
}

static int sgm41511_write_byte(struct sgm41511 *sgm, u8 reg, u8 data)
{
	int ret;

	mutex_lock(&sgm->i2c_rw_lock);
	ret = __sgm41511_write_reg(sgm, reg, data);
	mutex_unlock(&sgm->i2c_rw_lock);

	if (ret)
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);

	return ret;
}

static int sgm41511_update_bits(struct sgm41511 *sgm, u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	mutex_lock(&sgm->i2c_rw_lock);
	ret = __sgm41511_read_reg(sgm, reg, &tmp);
	if (ret) {
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);
		goto out;
	}

	tmp &= ~mask;
	tmp |= data & mask;

	ret = __sgm41511_write_reg(sgm, reg, tmp);
	if (ret)
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);

out:
	mutex_unlock(&sgm->i2c_rw_lock);
	return ret;
}

static int sgm41511_enable_otg(struct sgm41511 *sgm)
{
	u8 val = REG01_OTG_ENABLE << REG01_OTG_CONFIG_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_01, REG01_OTG_CONFIG_MASK,
					val);

}

static int sgm41511_disable_otg(struct sgm41511 *sgm)
{
	u8 val = REG01_OTG_DISABLE << REG01_OTG_CONFIG_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_01, REG01_OTG_CONFIG_MASK,
					val);

}

static int sgm41511_enable_charger(struct sgm41511 *sgm)
{
	int ret;
	u8 val = REG01_CHG_ENABLE << REG01_CHG_CONFIG_SHIFT;

	ret =
		sgm41511_update_bits(sgm, SGM41511_REG_01, REG01_CHG_CONFIG_MASK, val);

	return ret;
}

static int sgm41511_disable_charger(struct sgm41511 *sgm)
{
	int ret;
	u8 val = REG01_CHG_DISABLE << REG01_CHG_CONFIG_SHIFT;

	ret =
		sgm41511_update_bits(sgm, SGM41511_REG_01, REG01_CHG_CONFIG_MASK, val);
	return ret;
}

int sgm41511_set_chargecurrent(struct sgm41511 *sgm, int curr)
{
	u8 ichg;

	if (curr < REG02_ICHG_BASE)
		curr = REG02_ICHG_BASE;

	ichg = (curr - REG02_ICHG_BASE) / REG02_ICHG_LSB;
	return sgm41511_update_bits(sgm, SGM41511_REG_02, REG02_ICHG_MASK,
					ichg << REG02_ICHG_SHIFT);

}

int sgm41511_set_term_current(struct sgm41511 *sgm, int curr)
{
	u8 iterm;

	if (curr < REG03_ITERM_BASE)
		curr = REG03_ITERM_BASE;

	iterm = (curr - REG03_ITERM_BASE) / REG03_ITERM_LSB;

	return sgm41511_update_bits(sgm, SGM41511_REG_03, REG03_ITERM_MASK,
					iterm << REG03_ITERM_SHIFT);
}
EXPORT_SYMBOL_GPL(sgm41511_set_term_current);

int sgm41511_set_prechg_current(struct sgm41511 *sgm, int curr)
{
	u8 iprechg;

	if (curr < REG03_IPRECHG_BASE)
		curr = REG03_IPRECHG_BASE;

	iprechg = (curr - REG03_IPRECHG_BASE) / REG03_IPRECHG_LSB;

	return sgm41511_update_bits(sgm, SGM41511_REG_03, REG03_IPRECHG_MASK,
					iprechg << REG03_IPRECHG_SHIFT);
}
EXPORT_SYMBOL_GPL(sgm41511_set_prechg_current);

int sgm41511_set_chargevolt(struct sgm41511 *sgm, int volt)
{
	u8 val;

	if (volt < REG04_VREG_BASE)
		volt = REG04_VREG_BASE;

	val = (volt - REG04_VREG_BASE) / REG04_VREG_LSB;
	return sgm41511_update_bits(sgm, SGM41511_REG_04, REG04_VREG_MASK,
					val << REG04_VREG_SHIFT);
}

int sgm41511_set_input_volt_limit(struct sgm41511 *sgm, int volt)
{
	u8 val;

	if (volt < REG06_VINDPM_BASE)
		volt = REG06_VINDPM_BASE;

	val = (volt - REG06_VINDPM_BASE) / REG06_VINDPM_LSB;
	return sgm41511_update_bits(sgm, SGM41511_REG_06, REG06_VINDPM_MASK,
					val << REG06_VINDPM_SHIFT);
}

int sgm41511_set_input_current_limit(struct sgm41511 *sgm, int curr)
{
	u8 val;

	if (curr < REG00_IINLIM_BASE)
		curr = REG00_IINLIM_BASE;

	val = (curr - REG00_IINLIM_BASE) / REG00_IINLIM_LSB;
	return sgm41511_update_bits(sgm, SGM41511_REG_00, REG00_IINLIM_MASK,
					val << REG00_IINLIM_SHIFT);
}

int sgm41511_set_watchdog_timer(struct sgm41511 *sgm, u8 timeout)
{
	u8 temp;

	temp = (u8) (((timeout -
				REG05_WDT_BASE) / REG05_WDT_LSB) << REG05_WDT_SHIFT);

	return sgm41511_update_bits(sgm, SGM41511_REG_05, REG05_WDT_MASK, temp);
}
EXPORT_SYMBOL_GPL(sgm41511_set_watchdog_timer);

int sgm41511_disable_watchdog_timer(struct sgm41511 *sgm)
{
	u8 val = REG05_WDT_DISABLE << REG05_WDT_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_05, REG05_WDT_MASK, val);
}
EXPORT_SYMBOL_GPL(sgm41511_disable_watchdog_timer);

int sgm41511_reset_watchdog_timer(struct sgm41511 *sgm)
{
	u8 val = REG01_WDT_RESET << REG01_WDT_RESET_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_01, REG01_WDT_RESET_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_reset_watchdog_timer);

int sgm41511_reset_chip(struct sgm41511 *sgm)
{
	int ret;
	u8 val = REG0B_REG_RESET << REG0B_REG_RESET_SHIFT;

	ret =
		sgm41511_update_bits(sgm, SGM41511_REG_0B, REG0B_REG_RESET_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sgm41511_reset_chip);

int sgm41511_enter_hiz_mode(struct sgm41511 *sgm)
{
	u8 val = REG00_HIZ_ENABLE << REG00_ENHIZ_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_00, REG00_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(sgm41511_enter_hiz_mode);

int sgm41511_exit_hiz_mode(struct sgm41511 *sgm)
{

	u8 val = REG00_HIZ_DISABLE << REG00_ENHIZ_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_00, REG00_ENHIZ_MASK, val);

}
EXPORT_SYMBOL_GPL(sgm41511_exit_hiz_mode);

int sgm41511_get_hiz_mode(struct sgm41511 *sgm, u8 *state)
{
	u8 val;
	int ret;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_00, &val);
	if (ret)
		return ret;
	*state = (val & REG00_ENHIZ_MASK) >> REG00_ENHIZ_SHIFT;

	return 0;
}
EXPORT_SYMBOL_GPL(sgm41511_get_hiz_mode);

static int sgm41511_enable_term(struct sgm41511 *sgm, bool enable)
{
	u8 val;
	int ret;

	if (enable)
		val = REG05_TERM_ENABLE << REG05_EN_TERM_SHIFT;
	else
		val = REG05_TERM_DISABLE << REG05_EN_TERM_SHIFT;

	ret = sgm41511_update_bits(sgm, SGM41511_REG_05, REG05_EN_TERM_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sgm41511_enable_term);

int sgm41511_set_boost_current(struct sgm41511 *sgm, int curr)
{
	u8 val;

	val = REG02_BOOST_LIM_0P5A;
	if (curr == BOOSTI_1200)
		val = REG02_BOOST_LIM_1P2A;

	return sgm41511_update_bits(sgm, SGM41511_REG_02, REG02_BOOST_LIM_MASK,
					val << REG02_BOOST_LIM_SHIFT);
}

int sgm41511_set_boost_voltage(struct sgm41511 *sgm, int volt)
{
	u8 val;

	if (volt == BOOSTV_4850)
		val = REG06_BOOSTV_4P85V;
	else if (volt == BOOSTV_5150)
		val = REG06_BOOSTV_5P15V;
	else if (volt == BOOSTV_5300)
		val = REG06_BOOSTV_5P3V;
	else
		val = REG06_BOOSTV_5V;

	return sgm41511_update_bits(sgm, SGM41511_REG_06, REG06_BOOSTV_MASK,
					val << REG06_BOOSTV_SHIFT);
}
EXPORT_SYMBOL_GPL(sgm41511_set_boost_voltage);

static int sgm41511_set_acovp_threshold(struct sgm41511 *sgm, int volt)
{
	u8 val;

	if (volt == VAC_OVP_14000)
		val = REG06_OVP_14P0V;
	else if (volt == VAC_OVP_10500)
		val = REG06_OVP_10P5V;
	else if (volt == VAC_OVP_6500)
		val = REG06_OVP_6P5V;
	else
		val = REG06_OVP_5P5V;

	return sgm41511_update_bits(sgm, SGM41511_REG_06, REG06_OVP_MASK,
					val << REG06_OVP_SHIFT);
}
EXPORT_SYMBOL_GPL(sgm41511_set_acovp_threshold);

static int sgm41511_set_stat_ctrl(struct sgm41511 *sgm, int ctrl)
{
	u8 val;

	val = ctrl;

	return sgm41511_update_bits(sgm, SGM41511_REG_00, REG00_STAT_CTRL_MASK,
					val << REG00_STAT_CTRL_SHIFT);
}

static int sgm41511_set_int_mask(struct sgm41511 *sgm, int mask)
{
	u8 val;

	val = mask;

	return sgm41511_update_bits(sgm, SGM41511_REG_0A, REG0A_INT_MASK_MASK,
					val << REG0A_INT_MASK_SHIFT);
}

static int sgm41511_enable_batfet(struct sgm41511 *sgm)
{
	const u8 val = REG07_BATFET_ON << REG07_BATFET_DIS_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_07, REG07_BATFET_DIS_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_enable_batfet);

static int sgm41511_disable_batfet(struct sgm41511 *sgm)
{
	const u8 val = REG07_BATFET_OFF << REG07_BATFET_DIS_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_07, REG07_BATFET_DIS_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_disable_batfet);

static int sgm41511_set_batfet_delay(struct sgm41511 *sgm, uint8_t delay)
{
	u8 val;

	if (delay == 0)
		val = REG07_BATFET_DLY_0S;
	else
		val = REG07_BATFET_DLY_10S;

	val <<= REG07_BATFET_DLY_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_07, REG07_BATFET_DLY_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_set_batfet_delay);

static int sgm41511_enable_safety_timer(struct sgm41511 *sgm)
{
	const u8 val = REG05_CHG_TIMER_ENABLE << REG05_EN_TIMER_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_05, REG05_EN_TIMER_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_enable_safety_timer);

static int sgm41511_disable_safety_timer(struct sgm41511 *sgm)
{
	const u8 val = REG05_CHG_TIMER_DISABLE << REG05_EN_TIMER_SHIFT;

	return sgm41511_update_bits(sgm, SGM41511_REG_05, REG05_EN_TIMER_MASK,
					val);
}
EXPORT_SYMBOL_GPL(sgm41511_disable_safety_timer);

static struct sgm41511_platform_data *sgm41511_parse_dt(struct device_node *np,
								struct sgm41511 *sgm)
{
	int ret;
	struct sgm41511_platform_data *pdata;

	pdata = devm_kzalloc(sgm->dev, sizeof(struct sgm41511_platform_data),
				 GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (of_property_read_string(np, "charger_name", &sgm->chg_dev_name) < 0) {
		sgm->chg_dev_name = "primary_chg";
		pr_warn("no charger name\n");
	}

	if (of_property_read_string(np, "eint_name", &sgm->eint_name) < 0) {
		sgm->eint_name = "chr_stat";
		pr_warn("no eint name\n");
	}

	sgm->chg_det_enable =
		of_property_read_bool(np, "sgm41511,charge-detect-enable");

	ret = of_property_read_u32(np, "sgm41511,usb-vlim", &pdata->usb.vlim);
	if (ret) {
		pdata->usb.vlim = 4500;
		pr_err("Failed to read node of sgm41511,usb-vlim\n");
	}

	ret = of_property_read_u32(np, "sgm41511,usb-ilim", &pdata->usb.ilim);
	if (ret) {
		pdata->usb.ilim = 2000;
		pr_err("Failed to read node of sgm41511,usb-ilim\n");
	}

	ret = of_property_read_u32(np, "sgm41511,usb-vreg", &pdata->usb.vreg);
	if (ret) {
		pdata->usb.vreg = 4200;
		pr_err("Failed to read node of sgm41511,usb-vreg\n");
	}

	ret = of_property_read_u32(np, "sgm41511,usb-ichg", &pdata->usb.ichg);
	if (ret) {
		pdata->usb.ichg = 2000;
		pr_err("Failed to read node of sgm41511,usb-ichg\n");
	}

	ret = of_property_read_u32(np, "sgm41511,stat-pin-ctrl",
					&pdata->statctrl);
	if (ret) {
		pdata->statctrl = 0;
		pr_err("Failed to read node of sgm41511,stat-pin-ctrl\n");
	}

	ret = of_property_read_u32(np, "sgm41511,precharge-current",
					&pdata->iprechg);
	if (ret) {
		pdata->iprechg = 180;
		pr_err("Failed to read node of sgm41511,precharge-current\n");
	}

	ret = of_property_read_u32(np, "sgm41511,termination-current",
					&pdata->iterm);
	if (ret) {
		pdata->iterm = 180;
		pr_err
			("Failed to read node of sgm41511,termination-current\n");
	}

	ret =
		of_property_read_u32(np, "sgm41511,boost-voltage",
				 &pdata->boostv);
	if (ret) {
		pdata->boostv = 5000;
		pr_err("Failed to read node of sgm41511,boost-voltage\n");
	}

	ret =
		of_property_read_u32(np, "sgm41511,boost-current",
				 &pdata->boosti);
	if (ret) {
		pdata->boosti = 1200;
		pr_err("Failed to read node of sgm41511,boost-current\n");
	}

	ret = of_property_read_u32(np, "sgm41511,vac-ovp-threshold",
					&pdata->vac_ovp);
	if (ret) {
		pdata->vac_ovp = 6500;
		pr_err("Failed to read node of sgm41511,vac-ovp-threshold\n");
	}

	return pdata;
}

static int sgm41511_get_charger_type(struct sgm41511 *sgm, int *type)
{
	int ret;

	u8 reg_val = 0;
	int vbus_stat = 0;
	int chg_type = POWER_SUPPLY_TYPE_UNKNOWN;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_08, &reg_val);

	if (ret)
		return ret;

	vbus_stat = (reg_val & REG08_VBUS_STAT_MASK);
	vbus_stat >>= REG08_VBUS_STAT_SHIFT;

	switch (vbus_stat) {

	case REG08_VBUS_TYPE_NONE:
		chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		break;
	case REG08_VBUS_TYPE_SDP:
		chg_type = POWER_SUPPLY_TYPE_USB;
		break;
	case REG08_VBUS_TYPE_CDP:
		chg_type = POWER_SUPPLY_TYPE_USB_CDP;
		break;
	case REG08_VBUS_TYPE_DCP:
		chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case REG08_VBUS_TYPE_UNKNOWN:
		chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		break;
	case REG08_VBUS_TYPE_NON_STD:
		chg_type = POWER_SUPPLY_TYPE_USB_FLOAT;
		break;
	default:
		chg_type = POWER_SUPPLY_TYPE_USB_FLOAT;
		break;
	}

	*type = chg_type;

	return 0;
}

static int sgm41511_get_chrg_stat(struct sgm41511 *sgm,
	int *chg_stat)
{
	int ret;
	u8 val;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_08, &val);
	if (!ret) {
		val = val & REG08_CHRG_STAT_MASK;
		val = val >> REG08_CHRG_STAT_SHIFT;
		*chg_stat = val;
	}

	return ret;
}

static int sgm41511_inform_charger_type(struct sgm41511 *sgm)
{
	int ret = 0;
	union power_supply_propval propval;

	if (!sgm->psy) {
		sgm->psy = power_supply_get_by_name("charger");
		if (!sgm->psy) {
			pr_err("%s Couldn't get psy\n", __func__);
			return -ENODEV;
		}
	}

	if (sgm->psy_usb_type != POWER_SUPPLY_TYPE_UNKNOWN)
		propval.intval = 1;
	else
		propval.intval = 0;

	ret = power_supply_set_property(sgm->psy, POWER_SUPPLY_PROP_ONLINE,
					&propval);

	if (ret < 0)
		pr_notice("inform power supply online failed:%d\n", ret);

	propval.intval = sgm->psy_usb_type;

	ret = power_supply_set_property(sgm->psy,
					POWER_SUPPLY_PROP_CHARGE_TYPE,
					&propval);

	if (ret < 0)
		pr_notice("inform power supply charge type failed:%d\n", ret);

	return ret;
}

static irqreturn_t sgm41511_irq_handler(int irq, void *data)
{
	int ret;
	u8 reg_val;
	bool prev_pg;
	int prev_chg_type;
	struct sgm41511 *sgm = (struct sgm41511 *)data;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_08, &reg_val);
	if (ret)
		return IRQ_HANDLED;

	prev_pg = sgm->power_good;

	sgm->power_good = !!(reg_val & REG08_PG_STAT_MASK);

	if (!prev_pg && sgm->power_good)
		pr_notice("adapter/usb inserted\n");
	else if (prev_pg && !sgm->power_good)
		pr_notice("adapter/usb removed\n");

	prev_chg_type = sgm->psy_usb_type;

	ret = sgm41511_get_charger_type(sgm, &sgm->psy_usb_type);
	if (!ret && prev_chg_type != sgm->psy_usb_type && sgm->chg_det_enable)
		sgm41511_inform_charger_type(sgm);

	return IRQ_HANDLED;
}

static int sgm41511_register_interrupt(struct sgm41511 *sgm)
{
	int ret = 0;
	/*struct device_node *np;

	np = of_find_node_by_name(NULL, sgm->eint_name);
	if (np) {
		sgm->irq = irq_of_parse_and_map(np, 0);
	} else {
		pr_err("couldn't get irq node\n");
		return -ENODEV;
	}

	pr_info("irq = %d\n", sgm->irq);*/

	if (! sgm->client->irq) {
		pr_info("sgm->client->irq is NULL\n");//remember to config dws
		return -ENODEV;
	}

	ret = devm_request_threaded_irq(sgm->dev, sgm->client->irq, NULL,
					sgm41511_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"ti_irq", sgm);
	if (ret < 0) {
		pr_err("request thread irq failed:%d\n", ret);
		return ret;
	}

	enable_irq_wake(sgm->irq);

	return 0;
}

static int sgm41511_init_device(struct sgm41511 *sgm)
{
	int ret;

	sgm41511_disable_watchdog_timer(sgm);

	ret = sgm41511_set_stat_ctrl(sgm, sgm->platform_data->statctrl);
	if (ret)
		pr_err("Failed to set stat pin control mode, ret = %d\n", ret);

	ret = sgm41511_set_prechg_current(sgm, sgm->platform_data->iprechg);
	if (ret)
		pr_err("Failed to set prechg current, ret = %d\n", ret);

	ret = sgm41511_set_term_current(sgm, sgm->platform_data->iterm);
	if (ret)
		pr_err("Failed to set termination current, ret = %d\n", ret);

	ret = sgm41511_set_boost_voltage(sgm, sgm->platform_data->boostv);
	if (ret)
		pr_err("Failed to set boost voltage, ret = %d\n", ret);

	ret = sgm41511_set_boost_current(sgm, sgm->platform_data->boosti);
	if (ret)
		pr_err("Failed to set boost current, ret = %d\n", ret);

	ret = sgm41511_set_acovp_threshold(sgm, sgm->platform_data->vac_ovp);
	if (ret)
		pr_err("Failed to set acovp threshold, ret = %d\n", ret);

	ret = sgm41511_set_int_mask(sgm,
					REG0A_IINDPM_INT_MASK |
					REG0A_VINDPM_INT_MASK);
	if (ret)
		pr_err("Failed to set vindpm and iindpm int mask\n");

	return 0;
}

static void determine_initial_status(struct sgm41511 *sgm)
{
	sgm41511_irq_handler(sgm->irq, (void *) sgm);
}

static int sgm41511_detect_device(struct sgm41511 *sgm)
{
	int ret;
	u8 data;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_0B, &data);
	if (!ret) {
		sgm->part_no = (data & REG0B_PN_MASK) >> REG0B_PN_SHIFT;
		sgm->e_part_no = (data & REG0B_SGMPART_MASK) >> REG0B_SGMPART_SHIFT;
		sgm->revision =
			(data & REG0B_DEV_REV_MASK) >> REG0B_DEV_REV_SHIFT;
	}
	ret = sgm41511_read_byte(sgm, SGM41511_REG_0C, &data);
	if (!ret) {
		sgm->reg0c_no = (data & REG0C_RESERVED_MASK) >> REG0C_RESERVED_SHIFT;
	}

	return ret;
}

static void sgm41511_dump_regs(struct sgm41511 *sgm)
{
	int addr;
	u8 val;
	int ret;

	for (addr = 0x0; addr <= 0x0B; addr++) {
		ret = sgm41511_read_byte(sgm, addr, &val);
		if (ret == 0)
			pr_err("Reg[%.2x] = 0x%.2x\n", addr, val);
	}
}

static ssize_t
sgm41511_show_registers(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct sgm41511 *sgm = dev_get_drvdata(dev);
	u8 addr;
	u8 val;
	u8 tmpbuf[200];
	int len;
	int idx = 0;
	int ret;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "sgm41511 Reg");
	for (addr = 0x0; addr <= 0x0B; addr++) {
		ret = sgm41511_read_byte(sgm, addr, &val);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,
						"Reg[%.2x] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static ssize_t
sgm41511_store_registers(struct device *dev,
			struct device_attribute *attr, const char *buf,
			size_t count)
{
	struct sgm41511 *sgm = dev_get_drvdata(dev);
	int ret;
	unsigned int reg;
	unsigned int val;

	ret = sscanf(buf, "%x %x", &reg, &val);
	if (ret == 2 && reg < 0x0B) {
		sgm41511_write_byte(sgm, (unsigned char) reg,
					(unsigned char) val);
	}

	return count;
}

static DEVICE_ATTR(registers, S_IRUGO | S_IWUSR, sgm41511_show_registers,
			sgm41511_store_registers);

static struct attribute *sgm41511_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group sgm41511_attr_group = {
	.attrs = sgm41511_attributes,
};

static int sgm41511_charging(struct charger_device *chg_dev, bool enable)
{

	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret = 0;
	u8 val;

	if (enable)
		ret = sgm41511_enable_charger(sgm);
	else
		ret = sgm41511_disable_charger(sgm);

	pr_err("%s charger %s\n", enable ? "enable" : "disable",
			!ret ? "successfully" : "failed");

	ret = sgm41511_read_byte(sgm, SGM41511_REG_01, &val);

	if (!ret)
		sgm->charge_enabled = !!(val & REG01_CHG_CONFIG_MASK);

	return ret;
}

static int sgm41511_plug_in(struct charger_device *chg_dev)
{

	int ret;

	ret = sgm41511_charging(chg_dev, true);

	if (ret)
		pr_err("Failed to enable charging:%d\n", ret);

	return ret;
}

static int sgm41511_plug_out(struct charger_device *chg_dev)
{
	int ret;

	ret = sgm41511_charging(chg_dev, false);

	if (ret)
		pr_err("Failed to disable charging:%d\n", ret);

	return ret;
}

static int sgm41511_dump_register(struct charger_device *chg_dev)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	sgm41511_dump_regs(sgm);

	return 0;
}

static int sgm41511_is_charging_enable(struct charger_device *chg_dev, bool *en)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	*en = sgm->charge_enabled;

	return 0;
}

static int sgm41511_get_charging_status(struct charger_device *chg_dev,
	int *chg_stat)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret;
	u8 val;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_08, &val);
	if (!ret) {
		val = val & REG08_CHRG_STAT_MASK;
		val = val >> REG08_CHRG_STAT_SHIFT;
		*chg_stat = val;
	}

	return ret;
}

static int sgm41511_is_charging_done(struct charger_device *chg_dev, bool *done)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret;
	u8 val;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_08, &val);
	if (!ret) {
		val = val & REG08_CHRG_STAT_MASK;
		val = val >> REG08_CHRG_STAT_SHIFT;
		*done = (val == REG08_CHRG_STAT_CHGDONE);
	}

	return ret;
}

static int sgm41511_set_ichg(struct charger_device *chg_dev, u32 curr)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("charge curr = %d\n", curr);

	return sgm41511_set_chargecurrent(sgm, curr / 1000);
}

static int sgm41511_get_ichg(struct charger_device *chg_dev, u32 *curr)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int ichg;
	int ret;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_02, &reg_val);
	if (!ret) {
		ichg = (reg_val & REG02_ICHG_MASK) >> REG02_ICHG_SHIFT;
		ichg = ichg * REG02_ICHG_LSB + REG02_ICHG_BASE;
		*curr = ichg * 1000;
	}

	return ret;
}

static int sgm41511_set_iterm(struct charger_device *chg_dev, u32 uA)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("termination curr = %d\n", uA);

	return sgm41511_set_term_current(sgm, uA / 1000);
}

static int sgm41511_get_min_ichg(struct charger_device *chg_dev, u32 *curr)
{
	*curr = 60 * 1000;

	return 0;
}

static int sgm41511_get_min_aicr(struct charger_device *chg_dev, u32 *uA)
{
	*uA = 100 * 1000;
	return 0;
}

static int sgm41511_set_vchg(struct charger_device *chg_dev, u32 volt)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("charge volt = %d\n", volt);

	return sgm41511_set_chargevolt(sgm, volt / 1000);
}

static int sgm41511_get_vchg(struct charger_device *chg_dev, u32 *volt)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int vchg;
	int ret;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_04, &reg_val);
	if (!ret) {
		vchg = (reg_val & REG04_VREG_MASK) >> REG04_VREG_SHIFT;
		vchg = vchg * REG04_VREG_LSB + REG04_VREG_BASE;
		*volt = vchg * 1000;
	}

	return ret;
}

static int sgm41511_get_ivl_state(struct charger_device *chg_dev, bool *in_loop)
{
	int ret = 0;
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_0A, &reg_val);
	if (!ret)
		*in_loop = (ret & REG0A_VINDPM_STAT_MASK) >> REG0A_VINDPM_STAT_SHIFT;

	return ret;
}

static int sgm41511_get_ivl(struct charger_device *chg_dev, u32 *volt)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int ivl;
	int ret;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_06, &reg_val);
	if (!ret) {
		ivl = (reg_val & REG06_VINDPM_MASK) >> REG06_VINDPM_SHIFT;
		ivl = ivl * REG06_VINDPM_LSB + REG06_VINDPM_BASE;
		*volt = ivl * 1000;
	}

	return ret;
}

static int sgm41511_set_ivl(struct charger_device *chg_dev, u32 volt)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("vindpm volt = %d\n", volt);

	return sgm41511_set_input_volt_limit(sgm, volt / 1000);

}

static int sgm41511_set_icl(struct charger_device *chg_dev, u32 curr)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("indpm curr = %d\n", curr);

	return sgm41511_set_input_current_limit(sgm, curr / 1000);
}

static int sgm41511_get_icl(struct charger_device *chg_dev, u32 *curr)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int icl;
	int ret;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_00, &reg_val);
	if (!ret) {
		icl = (reg_val & REG00_IINLIM_MASK) >> REG00_IINLIM_SHIFT;
		icl = icl * REG00_IINLIM_LSB + REG00_IINLIM_BASE;
		*curr = icl * 1000;
	}

	return ret;

}

static int sgm41511_enable_te(struct charger_device *chg_dev, bool en)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	pr_err("enable_term = %d\n", en);

	return sgm41511_enable_term(sgm, en);
}

static int sgm41511_kick_wdt(struct charger_device *chg_dev)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	return sgm41511_reset_watchdog_timer(sgm);
}

static int sgm41511_set_otg(struct charger_device *chg_dev, bool en)
{
	int ret;
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	if (en)
		ret = sgm41511_enable_otg(sgm);
	else
		ret = sgm41511_disable_otg(sgm);

	pr_err("%s OTG %s\n", en ? "enable" : "disable",
			!ret ? "successfully" : "failed");

	return ret;
}

static int sgm41511_set_safety_timer(struct charger_device *chg_dev, bool en)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret;

	if (en)
		ret = sgm41511_enable_safety_timer(sgm);
	else
		ret = sgm41511_disable_safety_timer(sgm);

	return ret;
}

static int sgm41511_is_safety_timer_enabled(struct charger_device *chg_dev,
						bool *en)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret;
	u8 reg_val;

	ret = sgm41511_read_byte(sgm, SGM41511_REG_05, &reg_val);

	if (!ret)
		*en = !!(reg_val & REG05_EN_TIMER_MASK);

	return ret;
}

/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 start*/
static int sgm41511_do_event(struct charger_device *chg_dev, u32 event, u32 args)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	if (!sgm->psy) {
		dev_notice(sgm->dev, "%s: cannot get psy\n", __func__);
		return -ENODEV;
	}

	switch (event) {
	case EVENT_FULL:
	case EVENT_RECHARGE:
	case EVENT_DISCHARGE:
		power_supply_changed(sgm->psy);
		break;
	default:
		break;
	}
	return 0;
}
/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 end*/

static int sgm41511_set_boost_ilmt(struct charger_device *chg_dev, u32 curr)
{
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);
	int ret;

	pr_err("otg curr = %d\n", curr);

	ret = sgm41511_set_boost_current(sgm, curr / 1000);

	return ret;
}

/*HS03s for DEVAL5625-1125 by wenyaqi at 20210607 start*/
static int sgm41511_set_hiz_mode(struct charger_device *chg_dev, bool en)
{
	int ret;
	struct sgm41511 *sgm = dev_get_drvdata(&chg_dev->dev);

	if (en)
		ret = sgm41511_enter_hiz_mode(sgm);
	else
		ret = sgm41511_exit_hiz_mode(sgm);

	pr_err("%s hiz mode %s\n", en ? "enable" : "disable",
			!ret ? "successfully" : "failed");

	return ret;
}
/*HS03s for DEVAL5625-1125 by wenyaqi at 20210607 end*/

static struct charger_ops sgm41511_chg_ops = {
	/* Normal charging */
	.plug_in = sgm41511_plug_in,
	.plug_out = sgm41511_plug_out,
	.dump_registers = sgm41511_dump_register,
	.enable = sgm41511_charging,
	.is_enabled = sgm41511_is_charging_enable,
	.get_charging_current = sgm41511_get_ichg,
	.set_charging_current = sgm41511_set_ichg,
	.get_input_current = sgm41511_get_icl,
	.set_input_current = sgm41511_set_icl,
	.get_constant_voltage = sgm41511_get_vchg,
	.set_constant_voltage = sgm41511_set_vchg,
	.kick_wdt = sgm41511_kick_wdt,
	.set_mivr = sgm41511_set_ivl,
	.get_mivr = sgm41511_get_ivl,
	.get_mivr_state = sgm41511_get_ivl_state,
	.is_charging_done = sgm41511_is_charging_done,
	.set_eoc_current = sgm41511_set_iterm,
	.enable_termination = sgm41511_enable_te,
	.reset_eoc_state = NULL,
	.get_min_charging_current = sgm41511_get_min_ichg,
	.get_min_input_current = sgm41511_get_min_aicr,

	/* Safety timer */
	.enable_safety_timer = sgm41511_set_safety_timer,
	.is_safety_timer_enabled = sgm41511_is_safety_timer_enabled,

	/* Power path */
	.enable_powerpath = NULL,
	.is_powerpath_enabled = NULL,

	/* OTG */
	.enable_otg = sgm41511_set_otg,
	.set_boost_current_limit = sgm41511_set_boost_ilmt,
	.enable_discharge = NULL,

	/* PE+/PE+20 */
	.send_ta_current_pattern = NULL,
	.set_pe20_efficiency_table = NULL,
	.send_ta20_current_pattern = NULL,
	.enable_cable_drop_comp = NULL,

	/* ADC */
	.get_tchg_adc = NULL,
	.get_ibus_adc = NULL,

	/* Event */
	.event = sgm41511_do_event,

	.get_chr_status = sgm41511_get_charging_status,
	/*HS03s for DEVAL5625-1125 by wenyaqi at 20210607 start*/
	.set_hiz_mode = sgm41511_set_hiz_mode,
	/*HS03s for DEVAL5625-1125 by wenyaqi at 20210607 end*/
};

static struct of_device_id sgm41511_charger_match_table[] = {
	{
	 .compatible = "sgm41511",
	 .data = &pn_data[PN_SGM41511],
	 },
	{},
};
MODULE_DEVICE_TABLE(of, sgm41511_charger_match_table);

/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 start*/
/* ======================= */
/* charger ic Power Supply Ops */
/* ======================= */

enum CHG_STATUS {
	CHG_STATUS_NOT_CHARGING = 0,
	CHG_STATUS_PRE_CHARGE,
	CHG_STATUS_FAST_CHARGING,
	CHG_STATUS_DONE,
};

static int charger_ic_get_online(struct sgm41511 *sgm,
				     bool *val)
{
	bool pwr_rdy = false;

	sgm41511_get_charger_type(sgm, &sgm->psy_usb_type);
	if(sgm->psy_usb_type != POWER_SUPPLY_TYPE_UNKNOWN)
		pwr_rdy = false;
	else
		pwr_rdy = true;

	dev_info(sgm->dev, "%s: online = %d\n", __func__, pwr_rdy);
	*val = pwr_rdy;
	return 0;
}

static int charger_ic_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct sgm41511 *sgm = power_supply_get_drvdata(psy);
	bool pwr_rdy = false;
	int ret = 0;
	int chr_status = 0;

	dev_dbg(sgm->dev, "%s: prop = %d\n", __func__, psp);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = charger_ic_get_online(sgm, &pwr_rdy);
		val->intval = pwr_rdy;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (sgm->psy_usb_type == POWER_SUPPLY_USB_TYPE_UNKNOWN)
		{
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		ret = sgm41511_get_chrg_stat(sgm, &chr_status);
		if (ret < 0) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		switch(chr_status) {
		case CHG_STATUS_NOT_CHARGING:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		case CHG_STATUS_PRE_CHARGE:
		case CHG_STATUS_FAST_CHARGING:
			if(sgm->charge_enabled)
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			break;
		case CHG_STATUS_DONE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			ret = -ENODATA;
			break;
		}
		break;
	default:
		ret = -ENODATA;
	}
	return ret;
}

static enum power_supply_property charger_ic_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static const struct power_supply_desc charger_ic_desc = {
	.properties		= charger_ic_properties,
	.num_properties		= ARRAY_SIZE(charger_ic_properties),
	.get_property		= charger_ic_get_property,
};

static char *charger_ic_supplied_to[] = {
	"battery",
	"mtk-master-charger"
};
/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 end*/

static int sgm41511_charger_remove(struct i2c_client *client);
static int sgm41511_charger_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct sgm41511 *sgm;
	const struct of_device_id *match;
	struct device_node *node = client->dev.of_node;
	struct power_supply_config charger_cfg = {};

	int ret = 0;

	sgm = devm_kzalloc(&client->dev, sizeof(struct sgm41511), GFP_KERNEL);
	if (!sgm)
		return -ENOMEM;

	client->addr = 0x6B;
	sgm->dev = &client->dev;
	sgm->client = client;

	i2c_set_clientdata(client, sgm);

	mutex_init(&sgm->i2c_rw_lock);

	ret = sgm41511_detect_device(sgm);
	if (ret) {
		pr_err("No sgm41511 device found!\n");
		return -ENODEV;
	}

	match = of_match_node(sgm41511_charger_match_table, node);
	if (match == NULL) {
		pr_err("device tree match not found\n");
		return -EINVAL;
	}

	/*HS03s for DEVAL5625-1795 by wenyaqi at 20210624 start*/
	if (sgm->part_no == *(int *)match->data && sgm->e_part_no == EXTRA_SGM41511 &&
		sgm->reg0c_no != REG0C_ETA6953)
	{
		pr_info("part match, hw:%s, devicetree:%s, extra part no:%d, reg0c_no=%d\n",
			pn_str[sgm->part_no], pn_str[*(int *) match->data], sgm->e_part_no, sgm->reg0c_no);
	} else {

		pr_info("part no match, hw:%s, devicetree:%s, extra part no:%d, reg0c_no=%d\n",
			pn_str[sgm->part_no], pn_str[*(int *) match->data], sgm->e_part_no, sgm->reg0c_no);
		sgm41511_charger_remove(client);
		return -EINVAL;
	}
	/*HS03s for DEVAL5625-1795 by wenyaqi at 20210624 end*/

	sgm->platform_data = sgm41511_parse_dt(node, sgm);

	if (!sgm->platform_data) {
		pr_err("No platform data provided.\n");
		return -EINVAL;
	}

	ret = sgm41511_init_device(sgm);
	if (ret) {
		pr_err("Failed to init device\n");
		return ret;
	}

	sgm41511_register_interrupt(sgm);

	sgm->chg_dev = charger_device_register(sgm->chg_dev_name,
							&client->dev, sgm,
							&sgm41511_chg_ops,
							&sgm41511_chg_props);
	if (IS_ERR_OR_NULL(sgm->chg_dev)) {
		ret = PTR_ERR(sgm->chg_dev);
		return ret;
	}

	ret = sysfs_create_group(&sgm->dev->kobj, &sgm41511_attr_group);
	if (ret)
		dev_err(sgm->dev, "failed to register sysfs. err: %d\n", ret);

	determine_initial_status(sgm);

	/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 start*/
	/* power supply register */
	memcpy(&sgm->psy_desc,
		&charger_ic_desc, sizeof(sgm->psy_desc));
	sgm->psy_desc.name = dev_name(&client->dev);

	charger_cfg.drv_data = sgm;
	charger_cfg.of_node = sgm->dev->of_node;
	charger_cfg.supplied_to = charger_ic_supplied_to;
	charger_cfg.num_supplicants = ARRAY_SIZE(charger_ic_supplied_to);
	sgm->psy = devm_power_supply_register(&client->dev,
					&sgm->psy_desc, &charger_cfg);
	if (IS_ERR(sgm->psy)) {
		dev_notice(&client->dev, "Fail to register power supply dev\n");
		ret = PTR_ERR(sgm->psy);
	}
	/*HS03s for SR-AL5625-01-511 by wenyaqi at 20210618 end*/

	pr_err("sgm41511 probe successfully, Part Num:%d-%d, Revision:%d\n!",
			sgm->part_no, sgm->e_part_no, sgm->revision);

	return 0;
}

static int sgm41511_charger_remove(struct i2c_client *client)
{
	struct sgm41511 *sgm = i2c_get_clientdata(client);

	mutex_destroy(&sgm->i2c_rw_lock);

	sysfs_remove_group(&sgm->dev->kobj, &sgm41511_attr_group);

	return 0;
}

static void sgm41511_charger_shutdown(struct i2c_client *client)
{

}

static struct i2c_driver sgm41511_charger_driver = {
	.driver = {
			.name = "sgm41511-charger",
			.owner = THIS_MODULE,
			.of_match_table = sgm41511_charger_match_table,
			},

	.probe = sgm41511_charger_probe,
	.remove = sgm41511_charger_remove,
	.shutdown = sgm41511_charger_shutdown,

};

module_i2c_driver(sgm41511_charger_driver);

MODULE_DESCRIPTION("SGM41511 Charger Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SGM");
