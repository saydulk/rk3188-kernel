/* drivers/power/rk2818_battery.c
 *
 * battery detect driver for the rk2818 
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

#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <mach/gpio.h>
#include <mach/adc.h>
#include <mach/iomux.h>

#if 0
#define DBG(x...)   printk(x)
#else
#define DBG(x...)
#endif

#if 0
#define NUM_BAT 	3
#define NUM_ELECTRICITY 	10

#define BAT_CAP_1500MAH		0
#define BAT_CAP_1200MAH		1
#define BAT_CAP_1100MAH		2

#define ELECTRICITY_1000MA 	0
#define ELECTRICITY_900MA 	1
#define ELECTRICITY_800MA 	2
#define ELECTRICITY_700MA 	3
#define ELECTRICITY_600MA 	4
#define ELECTRICITY_500MA 	5
#define ELECTRICITY_400MA 	6
#define ELECTRICITY_300MA 	7
#define ELECTRICITY_200MA 	8
#define ELECTRICITY_100MA 	9

#define BAT_SELECT	BAT_CAP_1200MAH
#define ELECTRICITY_SELECT	ELECTRICITY_200MA

//about 10 minutes before battery is exhaust for different bat and electricity
static int BatMinVoltage[NUM_BAT][NUM_ELECTRICITY] = 
{
{3410, 3450, 3480, 3460, 3480, 3500, 3510, 3470, 3420, 3430},
{3360, 3400, 3410, 3400, 3430, 3430, 3460, 3480, 3440, 3330},
{3360, 3400, 3410, 3410, 3440, 3460, 3480, 3470, 3440, 3360},
};

#define BATT_MAX_VOL_VALUE	4300
#define BATT_NOMAL_VOL_VALUE  3900
#define	BATT_ZERO_VOL_VALUE  BatMinVoltage[BAT_SELECT][ELECTRICITY_SELECT]
#endif

/*******************���²��������޸�******************************/
#define	TIMER_MS_COUNTS		50		//��ʱ���ĳ���ms
#define	SLOPE_SECOND_COUNTS	120		//ͳ�Ƶ�ѹб�ʵ�ʱ����s
#define	TIME_UPDATE_STATUS	3000	//���µ��״̬��ʱ����ms
#define BATT_MAX_VOL_VALUE	4200	//����ʱ�ĵ�ص�ѹ	
#define	BATT_ZERO_VOL_VALUE  3400	//�ػ�ʱ�ĵ�ص�ѹ
#define BATT_NOMAL_VOL_VALUE  3800
#define	THRESHOLD_VOLTAGE_HIGH		3850
#define	THRESHOLD_VOLTAGE_MID		3550
#define	THRESHOLD_VOLTAGE_LOW		BATT_ZERO_VOL_VALUE
#define	THRESHOLD_SLOPE_HIGH		10	//б��ֵ = ��ѹ���͵��ٶ�
#define	THRESHOLD_SLOPE_MID			5	//<	THRESHOLD_SLOPE_HIGH	
#define	THRESHOLD_SLOPE_LOW			0	//< THRESHOLD_SLOPE_MID


#define CHN_BAT_ADC 	0
#define CHN_USB_ADC 	2
#define BATT_LEVEL_EMPTY	0
#define BATT_PRESENT_TRUE	 1
#define BATT_PRESENT_FALSE  0
#define BAT_1V2_VALUE	1422
#define KEY_CHARGEOK_PIN	RK2818_PIN_PH6

static int gBatStatus =  POWER_SUPPLY_STATUS_UNKNOWN;
static int gBatHealth = POWER_SUPPLY_HEALTH_GOOD;
static int gBatLastCapacity = 0;
static int gBatPresent = BATT_PRESENT_TRUE;
static int gBatVoltage =  BATT_NOMAL_VOL_VALUE;
static int gBatCapacity = ((BATT_NOMAL_VOL_VALUE-BATT_ZERO_VOL_VALUE)*100/(BATT_MAX_VOL_VALUE-BATT_ZERO_VOL_VALUE));



/*************************************************************/
#define LODER_CHARGE_LEVEL		0	//����״̬�ȼ�
#define	LODER_HIGH_LEVEL		1
#define	LODER_MID_LEVEL			2
#define	LOADER_RELEASE_LEVEL	3	//��ؼ����ľ�״̬

#define	SLOPE_HIGH_LEVEL		0	//��ѹ�仯б�ʵȼ�
#define	SLOPE_MID_LEVEL			1
#define	SLOPE_LOW_LEVEL			2

#define	VOLTAGE_HIGH_LEVEL		0	//��ѹ�ߵ͵ȼ�
#define	VOLTAGE_MID_LEVEL		1
#define	VOLTAGE_LOW_LEVEL		2
#define	VOLTAGE_RELEASE_LEVEL	3

#define	NUM_VOLTAGE_SAMPLE	((1000*SLOPE_SECOND_COUNTS) / TIMER_MS_COUNTS)	//�洢�Ĳ��������
int gBatVoltageSamples[NUM_VOLTAGE_SAMPLE];
int gBatSlopeValue = 0;
int	gBatVoltageValue[2]={0,0};
int *pSamples = &gBatVoltageSamples[0];		//������ָ��
int gFlagLoop = 0;		//�����㹻��־
int gNumSamples = 0;

int gBatSlopeLevel = SLOPE_LOW_LEVEL;
int gBatVoltageLevel = VOLTAGE_MID_LEVEL;
int gBatLastLoaderLevel = LODER_MID_LEVEL;
int gBatLoaderLevel = LODER_MID_LEVEL;	


extern int dwc_vbus_status(void);

struct rk2818_battery_data {
	int irq;
	spinlock_t lock;
	struct work_struct 	timer_work;
	struct timer_list timer;
	struct power_supply battery;
	struct power_supply usb;
	struct power_supply ac;
	
	int adc_bat_divider;
	int bat_max;
	int bat_min;
};


/* temporary variable used between rk2818_battery_probe() and rk2818_battery_open() */
static struct rk2818_battery_data *gBatteryData;

enum {
	BATTERY_STATUS          = 0,
	BATTERY_HEALTH          = 1,
	BATTERY_PRESENT         = 2,
	BATTERY_CAPACITY        = 3,
	BATTERY_AC_ONLINE       = 4,
	BATTERY_STATUS_CHANGED	= 5,
	AC_STATUS_CHANGED   	= 6,
	BATTERY_INT_STATUS	    = 7,
	BATTERY_INT_ENABLE	    = 8,
};

typedef enum {
	CHARGER_BATTERY = 0,
	CHARGER_USB,
	CHARGER_AC
} charger_type_t;

static int rk2818_get_charge_status(void)
{
#if 0
	return dwc_vbus_status();
#else
	//DBG("gAdcValue[CHN_USB_ADC]=%d\n",gAdcValue[CHN_USB_ADC]);
	if(gAdcValue[CHN_USB_ADC] > 100)
	return 1;
	else
	return 0;
#endif
}

static void rk2818_get_bat_status(struct rk2818_battery_data *bat)
{
	if(rk2818_get_charge_status() == 1)
	{
		if(gpio_get_value (KEY_CHARGEOK_PIN) == 1) //CHG_OK ==0 
		gBatStatus = POWER_SUPPLY_STATUS_FULL;
		else
		gBatStatus = POWER_SUPPLY_STATUS_CHARGING;		
	}
	else
	gBatStatus = POWER_SUPPLY_STATUS_NOT_CHARGING;	
}

static void rk2818_get_bat_health(struct rk2818_battery_data *bat)
{
	gBatHealth = POWER_SUPPLY_HEALTH_GOOD;
}

static void rk2818_get_bat_present(struct rk2818_battery_data *bat)
{
	if(gBatVoltage < bat->bat_min)
	gBatPresent = 0;
	else
	gBatPresent = 1;
}

static void rk2818_get_bat_voltage(struct rk2818_battery_data *bat)
{
	unsigned long value;
	int i,*pSamp,*pStart = &gBatVoltageSamples[0];
	int temp[2] = {0,0};
	value = gAdcValue[CHN_BAT_ADC];
	gBatVoltage = (value * BAT_1V2_VALUE * 2)/gAdcValue[3];	// channel 3 is about 1.42v,need modified
	*pSamples = gBatVoltage;
	if((++pSamples - pStart) > NUM_VOLTAGE_SAMPLE)
	{
		pSamples = pStart;
		gFlagLoop = 1;
	}

	//compute the average voltage after samples-count is larger than NUM_VOLTAGE_SAMPLE
	if(gFlagLoop)
	{
		pSamp = pSamples;
		for(i=0; i<(NUM_VOLTAGE_SAMPLE >> 1); i++)
		{
			temp[0] += *pSamp;
			if((++pSamp - pStart) > NUM_VOLTAGE_SAMPLE)
			pSamp = pStart;
		}
		
		gBatVoltageValue[0] = temp[0] / (NUM_VOLTAGE_SAMPLE >> 1);
		for(i=0; i<(NUM_VOLTAGE_SAMPLE >> 1); i++)
		{
			temp[1] += *pSamp;
			if((++pSamp - pStart) > NUM_VOLTAGE_SAMPLE)
			pSamp = pStart;
		}
		
		gBatVoltageValue[1] = temp[1] / (NUM_VOLTAGE_SAMPLE >> 1);

		gBatSlopeValue = gBatVoltageValue[0] - gBatVoltageValue[1];	
		//DBG("gBatSlopeValue=%d,gBatVoltageValue[1]=%d\n",gBatSlopeValue,gBatVoltageValue[1]);
		if(gBatSlopeValue >= 0)	//�õ�״̬
		{

			if(gBatVoltageValue[1] >= THRESHOLD_VOLTAGE_HIGH)
			gBatVoltageLevel = 	VOLTAGE_HIGH_LEVEL;	
			else if((gBatVoltageValue[1] >= THRESHOLD_VOLTAGE_MID) && (gBatVoltageValue[1] < THRESHOLD_VOLTAGE_HIGH))
			gBatVoltageLevel = 	VOLTAGE_MID_LEVEL;
			else if((gBatVoltageValue[1] >= THRESHOLD_VOLTAGE_LOW) && (gBatVoltageValue[1] < THRESHOLD_VOLTAGE_MID))
			gBatVoltageLevel = VOLTAGE_LOW_LEVEL;
			else
			gBatVoltageLevel = VOLTAGE_RELEASE_LEVEL;

			if(gBatSlopeValue >= THRESHOLD_SLOPE_HIGH)
			gBatSlopeLevel = SLOPE_HIGH_LEVEL;	
			else if((gBatSlopeValue >= THRESHOLD_SLOPE_MID) && (gBatSlopeValue < THRESHOLD_SLOPE_HIGH))
			gBatSlopeLevel = SLOPE_MID_LEVEL;	
			else if(gBatSlopeValue >= THRESHOLD_SLOPE_LOW)
			gBatSlopeLevel = SLOPE_LOW_LEVEL;
			
			/*��ѹ����б�ʸߡ� ��ѹ����б�ʸ߻���*/
			if(((gBatVoltageLevel == VOLTAGE_MID_LEVEL) && (gBatSlopeLevel == SLOPE_HIGH_LEVEL)) \
				|| ((gBatVoltageLevel == VOLTAGE_HIGH_LEVEL) && ((gBatSlopeLevel == SLOPE_HIGH_LEVEL) || (gBatSlopeLevel == SLOPE_MID_LEVEL))))
			{
				gBatLoaderLevel = LODER_HIGH_LEVEL;
				//DBG("gBatLoaderLevel = LODER_HIGH_LEVEL\n");
			}
			
			/*��ѹ����б���л�͡���ѹ����б�ʵ͡� ��ѹ����б�ʵ�*/
			else if(((gBatVoltageLevel != VOLTAGE_RELEASE_LEVEL) && (gBatSlopeLevel == SLOPE_LOW_LEVEL)) \
				|| ((gBatVoltageLevel == VOLTAGE_MID_LEVEL) && (gBatSlopeLevel == SLOPE_MID_LEVEL)))
			{
				gBatLoaderLevel = LODER_MID_LEVEL;
				//DBG("gBatLoaderLevel = LODER_MID_LEVEL\n");
			}
			
			/*��ѹ����б�ʸ߻��С� ��ѹ����*/
			else if(((gBatVoltageLevel == VOLTAGE_LOW_LEVEL) && ((gBatSlopeLevel == SLOPE_MID_LEVEL) || (gBatSlopeLevel == SLOPE_MID_LEVEL))) \
				|| (gBatVoltageLevel == VOLTAGE_RELEASE_LEVEL))
			{
				gBatLoaderLevel = LOADER_RELEASE_LEVEL;	//����Ѻľ�
				//DBG("gBatLoaderLevel = LOADER_RELEASE_LEVEL\n");
			}

		}
		else	//���״̬
		{
			//to do
			gBatLoaderLevel = LODER_CHARGE_LEVEL;
		}
		
	}
	
}

static void rk2818_get_bat_capacity(struct rk2818_battery_data *bat)
{
	if(gFlagLoop)
	{
		//���ָ��ر�Сʱ�����������ʱ������������ֵ
		if((gBatLastCapacity ==0) \
			|| (gBatLoaderLevel <= gBatLastLoaderLevel) \
			|| ((gBatLoaderLevel > gBatLastLoaderLevel)&&(gBatCapacity <= gBatLastCapacity)))	
		{
			gBatCapacity = ((gBatVoltageValue[1] - bat->bat_min) * 100) / (bat->bat_max - bat->bat_min);
			if(gBatCapacity >= 100)
			gBatCapacity = 100;
			else if(gBatCapacity < 0)
			gBatCapacity = 0;
			gBatLastCapacity = gBatCapacity;
			gBatLastLoaderLevel = gBatLoaderLevel;
		}

	}
	else
	{
		gBatCapacity = ((gBatVoltage - bat->bat_min) * 100) / (bat->bat_max - bat->bat_min);
		if(gBatCapacity >= 100)
			gBatCapacity = 100;
		else if(gBatCapacity < 0)
			gBatCapacity = 0;
	}
}


static void rk2818_battery_timer_work(struct work_struct *work)
{	
	rk2818_get_bat_status(gBatteryData);
	rk2818_get_bat_health(gBatteryData);
	rk2818_get_bat_present(gBatteryData);
	rk2818_get_bat_voltage(gBatteryData);
	rk2818_get_bat_capacity(gBatteryData);

	if(++gNumSamples > TIME_UPDATE_STATUS/TIMER_MS_COUNTS)
	{
		gNumSamples = 0;
		if(gFlagLoop == 1)	//update battery parameter after adc
		{
			if(!( strstr(saved_command_line,"nfsroot=") ) )
			{
				power_supply_changed(&gBatteryData->battery);
				power_supply_changed(&gBatteryData->usb);
				power_supply_changed(&gBatteryData->ac);
			}
			else
			{
				DBG("voltage has changed\n");
				DBG("gBatStatus=%d,gBatHealth=%d,gBatPresent=%d\n",gBatStatus,gBatHealth,gBatPresent);
				if(gBatVoltageValue[1] == 0)
				DBG("gBatVoltage=%d\n",gBatVoltage);
				else
				DBG("gBatVoltage=%d\n",gBatVoltageValue[1]);
				if(gBatLastCapacity == 0)
				DBG("gBatCapacity=%d%%\n",gBatCapacity);
				else
				DBG("gBatCapacity=%d%%\n",gBatLastCapacity);
				DBG("gBatSlopeValue == %d\n",gBatSlopeValue);
				if(gBatLoaderLevel == LODER_CHARGE_LEVEL)
				DBG("gBatLoaderLevel == LODER_CHARGE_LEVEL\n");
				else if(gBatLoaderLevel == LODER_HIGH_LEVEL)
				DBG("gBatLoaderLevel == LODER_HIGH_LEVEL\n");	
				else if(gBatLoaderLevel == LODER_MID_LEVEL)
				DBG("gBatLoaderLevel == LODER_MID_LEVEL\n");	
				else if(gBatLoaderLevel == LOADER_RELEASE_LEVEL)
				DBG("gBatLoaderLevel == LOADER_RELEASE_LEVEL\n");	
			}

		}

	}
}


static void rk2818_batscan_timer(unsigned long data)
{
	gBatteryData->timer.expires  = jiffies + msecs_to_jiffies(TIMER_MS_COUNTS);
	add_timer(&gBatteryData->timer);
	schedule_work(&gBatteryData->timer_work);	
}


static int rk2818_usb_get_property(struct power_supply *psy, 
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	charger_type_t charger;
	charger =  CHARGER_USB;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->type == POWER_SUPPLY_TYPE_USB)
			val->intval = (charger ==  CHARGER_AC ? 1 : 0);
		else
			val->intval = 0;
		break;
	default:
		return -EINVAL;
	}
	
	return 0;

}


static int rk2818_ac_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
//	struct rk2818_battery_data *data = container_of(psy,
//		struct rk2818_battery_data, ac);
	int ret = 0;
	charger_type_t charger;
	charger =  CHARGER_USB;
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->type == POWER_SUPPLY_TYPE_MAINS)
			val->intval = (charger ==  CHARGER_AC ? 1 : 0);
		else if (psy->type == POWER_SUPPLY_TYPE_USB)
			val->intval = (charger ==  CHARGER_USB ? 1 : 0);
		else
			val->intval = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int rk2818_battery_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct rk2818_battery_data *data = container_of(psy,
		struct rk2818_battery_data, battery);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = gBatStatus;
		DBG("gBatStatus=%d\n",val->intval);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = gBatHealth;
		DBG("gBatHealth=%d\n",val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = gBatPresent;
		DBG("gBatPresent=%d\n",val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if(gBatVoltageValue[1] == 0)
		val ->intval = gBatVoltage;
		else
		val ->intval = gBatVoltageValue[1];
		DBG("gBatVoltage=%d\n",val->intval);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;	
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if(gBatLastCapacity == 0)
		val->intval = gBatCapacity;
		else
		val->intval = gBatLastCapacity;	
		DBG("gBatCapacity=%d%%\n",val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = data->bat_max;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = data->bat_min;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static enum power_supply_property rk2818_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
};

static enum power_supply_property rk2818_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};


static enum power_supply_property rk2818_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};
#if 0
static irqreturn_t rk2818_battery_interrupt(int irq, void *dev_id)
{

	unsigned long irq_flags;
	struct rk2818_battery_data *data = dev_id;
	uint32_t status;

	spin_lock_irqsave(&data->lock, irq_flags);
	/* read status flags, which will clear the interrupt */
	//status = RK2818_BATTERY_READ(data, BATTERY_INT_STATUS);
	status &= BATTERY_INT_MASK;

	if (status & BATTERY_STATUS_CHANGED)
		power_supply_changed(&data->battery);
	if (status & AC_STATUS_CHANGED)
		power_supply_changed(&data->ac);

	spin_unlock_irqrestore(&data->lock, irq_flags);
	return status ? IRQ_HANDLED : IRQ_NONE;

	return IRQ_HANDLED;
}
#endif

static int rk2818_battery_probe(struct platform_device *pdev)
{
	int ret;
	struct rk2818_battery_data *data;

	ret = gpio_request(KEY_CHARGEOK_PIN, NULL);
	if (ret) {
		printk("failed to request charge_ok key gpio\n");
		goto err_free_gpio;
	}
	
	gpio_pull_updown(KEY_CHARGEOK_PIN, GPIOPullUp);//important
	ret = gpio_direction_input(KEY_CHARGEOK_PIN);
	if (ret) {
		printk("failed to set gpio charge_ok input\n");
		goto err_free_gpio;
	}
	
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		ret = -ENOMEM;
		goto err_data_alloc_failed;
	}
	spin_lock_init(&data->lock);
	
	memset(gBatVoltageSamples, 0, sizeof(gBatVoltageSamples));
	
	data->battery.properties = rk2818_battery_props;
	data->battery.num_properties = ARRAY_SIZE(rk2818_battery_props);
	data->battery.get_property = rk2818_battery_get_property;
	data->battery.name = "battery";
	data->battery.type = POWER_SUPPLY_TYPE_BATTERY;
	data->adc_bat_divider = 414;
	data->bat_max = BATT_MAX_VOL_VALUE;
	data->bat_min = BATT_ZERO_VOL_VALUE;
	DBG("bat_min = %d\n",data->bat_min);
	
	data->usb.properties = rk2818_usb_props;
	data->usb.num_properties = ARRAY_SIZE(rk2818_ac_props);
	data->usb.get_property = rk2818_usb_get_property;
	data->usb.name = "usb";
	data->usb.type = POWER_SUPPLY_TYPE_USB;

	data->ac.properties = rk2818_ac_props;
	data->ac.num_properties = ARRAY_SIZE(rk2818_ac_props);
	data->ac.get_property = rk2818_ac_get_property;
	data->ac.name = "ac";
	data->ac.type = POWER_SUPPLY_TYPE_MAINS;
	
	ret = power_supply_register(&pdev->dev, &data->ac);
	if (ret)
	{
		printk(KERN_INFO "fail to power_supply_register\n");
		goto err_ac_failed;
	}

	ret = power_supply_register(&pdev->dev, &data->usb);
	if (ret)
	{
		printk(KERN_INFO "fail to power_supply_register\n");
		goto err_usb_failed;
	}

	ret = power_supply_register(&pdev->dev, &data->battery);
	if (ret)
	{
		printk(KERN_INFO "fail to power_supply_register\n");
		goto err_battery_failed;
	}
	platform_set_drvdata(pdev, data);

	INIT_WORK(&data->timer_work, rk2818_battery_timer_work);
	gBatteryData = data;
	
	setup_timer(&data->timer, rk2818_batscan_timer, (unsigned long)data);
	data->timer.expires  = jiffies+100;
	add_timer(&data->timer);
	printk(KERN_INFO "rk2818_battery: driver initialized\n");
	
	return 0;

err_battery_failed:
	power_supply_unregister(&data->usb);
err_usb_failed:
	power_supply_unregister(&data->ac);
err_ac_failed:
	kfree(data);
err_data_alloc_failed:
err_free_gpio:
	gpio_free(KEY_CHARGEOK_PIN);
	return ret;
}

static int rk2818_battery_remove(struct platform_device *pdev)
{
	struct rk2818_battery_data *data = platform_get_drvdata(pdev);

	power_supply_unregister(&data->battery);
	power_supply_unregister(&data->ac);
	gpio_free(KEY_CHARGEOK_PIN);
	free_irq(data->irq, data);
	kfree(data);
	gBatteryData = NULL;
	return 0;
}

static struct platform_driver rk2818_battery_device = {
	.probe		= rk2818_battery_probe,
	.remove		= rk2818_battery_remove,
	.driver = {
		.name = "rk2818-battery",
		.owner	= THIS_MODULE,
	}
};

static int __init rk2818_battery_init(void)
{
	return platform_driver_register(&rk2818_battery_device);
}

static void __exit rk2818_battery_exit(void)
{
	platform_driver_unregister(&rk2818_battery_device);
}

module_init(rk2818_battery_init);
module_exit(rk2818_battery_exit);

MODULE_DESCRIPTION("Battery detect driver for the rk2818");
MODULE_AUTHOR("luowei lw@rock-chips.com");
MODULE_LICENSE("GPL");
