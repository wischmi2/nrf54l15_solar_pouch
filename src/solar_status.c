#include "solar_status.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(solar_status, LOG_LEVEL_INF);

#define CHARGER_NODE DT_NODELABEL(npm1300_charger)

#define CHG_STATUS_BATTERY_DETECTED BIT(0)
#define CHG_STATUS_COMPLETE         BIT(1)
#define CHG_STATUS_TRICKLE          BIT(2)
#define CHG_STATUS_CC               BIT(3)
#define CHG_STATUS_CV               BIT(4)
#define VBUS_PRESENT                BIT(0)

/* AEM10330 STO_CFG=LLLL (v3): charge to 4.05 V, ready 3.50 V, cut-off 3.00 V.
 * USB via nPM1300 still terminates at 4.20 V, so a USB-full cell is AEM-BLOCKED.
 */
#define AEM_VOVCH_MV        4050
#define AEM_VCHRDY_MV       3500
#define AEM_VOVDIS_MV       3000
#define AEM_IBAT_CHG_MA     3
#define VBAT_PRESENT_MIN_MV 2500
#define VBAT_PRESENT_MAX_MV 4500

/* AEM charge is detected by VBAT rising, not nPM1300 IBAT. ADC noise is
 * about 5-10 mV, so require a sustained climb over 30 s.
 */
#define STATUS_PERIOD_MS  2000
#define AEM_RISE_WINDOW_S 30
#define AEM_RISE_MV       15
#define AEM_RISE_HOLD_MV  8
#define AEM_RISE_SPIKE_MV 40
#define AEM_RISE_SAMPLES  ((AEM_RISE_WINDOW_S * 1000) / STATUS_PERIOD_MS)

#define VBAT_EMPTY_MV 3300
#define VBAT_FULL_MV  4200

#define SOLAR_STACK_SIZE 2048
#define SOLAR_PRIORITY   10

enum charge_state {
    CHG_NO_BATTERY = 0,
    CHG_IDLE,
    CHG_TRICKLE,
    CHG_CC,
    CHG_CV,
    CHG_COMPLETE,
};

struct solar_snapshot {
    uint16_t vbat_mv;
    uint8_t soc_pct;
    int16_t ibat_ma;
    const char *charge;
    bool vbus;
    bool usb_cc;
    const char *source;
    bool aem;
    const char *aem_window;
    int vbat_rise_mv;
};

static const struct device *charger = DEVICE_DT_GET(CHARGER_NODE);
static struct k_mutex snap_lock;
static struct solar_snapshot snap = {
    .charge = "NO-BATT",
    .source = "BATTERY",
    .aem_window = "AEM-CUTOFF",
};

static uint16_t vbat_hist[AEM_RISE_SAMPLES];
static uint8_t vbat_hist_count;
static uint8_t vbat_hist_head;
static uint16_t vbat_prev_mv;
static int vbat_rise_dv;
static bool aem_active;

K_KERNEL_STACK_DEFINE(solar_stack, SOLAR_STACK_SIZE);
static struct k_work_q solar_wq;
static struct k_work_delayable sample_work;

static int32_t sensor_to_milli(const struct sensor_value *v)
{
    int64_t micro = ((int64_t)v->val1 * 1000000LL) + (int64_t)v->val2;

    return (int32_t)(micro / 1000LL);
}

static uint8_t soc_from_mv(uint16_t mv)
{
    if (mv <= VBAT_EMPTY_MV)
    {
        return 0;
    }
    if (mv >= VBAT_FULL_MV)
    {
        return 100;
    }
    return (uint8_t)(((uint32_t)(mv - VBAT_EMPTY_MV) * 100U) / (VBAT_FULL_MV - VBAT_EMPTY_MV));
}

static enum charge_state decode_state(int status, uint16_t vbat_mv)
{
    if (status & CHG_STATUS_COMPLETE)
    {
        return CHG_COMPLETE;
    }
    if (status & CHG_STATUS_CV)
    {
        return CHG_CV;
    }
    if (status & CHG_STATUS_CC)
    {
        return CHG_CC;
    }
    if (status & CHG_STATUS_TRICKLE)
    {
        return CHG_TRICKLE;
    }
    /* BIT(0) BATTERYDETECTED is reserved/always 0 on this nPM1300 path.
     * A real LiPo voltage means the cell is present even when idle.
     */
    if ((status & CHG_STATUS_BATTERY_DETECTED) ||
        ((vbat_mv >= VBAT_PRESENT_MIN_MV) && (vbat_mv <= VBAT_PRESENT_MAX_MV)))
    {
        return CHG_IDLE;
    }
    return CHG_NO_BATTERY;
}

static const char *charge_str(enum charge_state state)
{
    switch (state)
    {
    case CHG_COMPLETE:
        return "COMPLETE";
    case CHG_CV:
        return "CV";
    case CHG_CC:
        return "CC";
    case CHG_TRICKLE:
        return "TRICKLE";
    case CHG_IDLE:
        return "IDLE";
    default:
        return "NO-BATT";
    }
}

static bool charge_active(enum charge_state state)
{
    return (state == CHG_TRICKLE) || (state == CHG_CC) || (state == CHG_CV);
}

static const char *aem_window_str(uint16_t mv)
{
    if (mv > AEM_VOVCH_MV)
    {
        return "AEM-BLOCKED";
    }
    if (mv >= AEM_VCHRDY_MV)
    {
        return "AEM-OK";
    }
    if (mv >= AEM_VOVDIS_MV)
    {
        return "AEM-LOW";
    }
    return "AEM-CUTOFF";
}

static const char *source_str(bool vbus, bool usb_cc, bool charging, bool aem)
{
    if (vbus)
    {
        if (usb_cc)
        {
            return charging ? "USB" : "USB-IDLE";
        }
        return charging ? "USB-NO-CC" : "USB-NO-CC-IDLE";
    }
    return aem ? "AEM" : "BATTERY";
}

static void vbat_hist_reset(void)
{
    vbat_hist_count = 0;
    vbat_hist_head = 0;
    vbat_prev_mv = 0;
    vbat_rise_dv = 0;
}

/* True when USB is out and VBAT climbed over the last 30 s. Large jumps
 * reset the window so a clip or unclip is not counted as harvest.
 */
static bool vbat_is_rising(uint16_t vbat_mv, bool vbus)
{
    uint16_t oldest;
    int threshold;
    int step;

    if (vbus)
    {
        vbat_hist_reset();
        return false;
    }

    if (vbat_prev_mv != 0)
    {
        step = (int)vbat_mv - (int)vbat_prev_mv;
        if ((step > AEM_RISE_SPIKE_MV) || (step < -AEM_RISE_SPIKE_MV))
        {
            vbat_hist_reset();
        }
    }
    vbat_prev_mv = vbat_mv;

    vbat_hist[vbat_hist_head] = vbat_mv;
    vbat_hist_head = (uint8_t)((vbat_hist_head + 1U) % AEM_RISE_SAMPLES);
    if (vbat_hist_count < AEM_RISE_SAMPLES)
    {
        vbat_hist_count++;
        vbat_rise_dv = 0;
        return false;
    }

    oldest = vbat_hist[vbat_hist_head];
    vbat_rise_dv = (int)vbat_mv - (int)oldest;
    threshold = aem_active ? AEM_RISE_HOLD_MV : AEM_RISE_MV;
    return vbat_rise_dv >= threshold;
}

static bool vbus_present(void)
{
    struct sensor_value val = {0};

    if (sensor_attr_get(charger,
                        (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
                        (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
                        &val) < 0)
    {
        return false;
    }
    return val.val1 != 0;
}

/* USB-C CC detect, not VBUS. The panel path has no CC pin. */
static bool usb_cc_present(void)
{
    struct sensor_value val = {0};

    if (sensor_attr_get(charger, SENSOR_CHAN_CURRENT, SENSOR_ATTR_UPPER_THRESH, &val) < 0)
    {
        return false;
    }
    return (val.val1 != 0) || (val.val2 != 0);
}

static void publish(const struct solar_snapshot *next)
{
    k_mutex_lock(&snap_lock, K_FOREVER);
    snap = *next;
    k_mutex_unlock(&snap_lock);
}

static void sample_handler(struct k_work *work)
{
    struct sensor_value v_bat = {0};
    struct sensor_value i_bat = {0};
    struct sensor_value chg = {0};
    struct sensor_value vbus = {0};
    struct solar_snapshot next = {0};
    enum charge_state state;
    int32_t mv;
    int32_t ma;
    bool rising;
    int ret;

    ARG_UNUSED(work);

    if (!device_is_ready(charger))
    {
        LOG_ERR("nPM1300 charger not ready");
        k_work_schedule_for_queue(&solar_wq, &sample_work, K_MSEC(STATUS_PERIOD_MS));
        return;
    }

    next.vbus = vbus_present();
    next.usb_cc = usb_cc_present();

    ret = sensor_sample_fetch(charger);
    if (ret < 0)
    {
        LOG_ERR("nPM1300 sample fetch failed (%d)", ret);
        k_work_schedule_for_queue(&solar_wq, &sample_work, K_MSEC(STATUS_PERIOD_MS));
        return;
    }

    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &v_bat);
    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &i_bat);
    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &chg);
    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &vbus);

    mv = sensor_to_milli(&v_bat);
    ma = sensor_to_milli(&i_bat);
    if (mv < 0)
    {
        mv = 0;
    }
    else if (mv > UINT16_MAX)
    {
        mv = UINT16_MAX;
    }
    if (ma > INT16_MAX)
    {
        ma = INT16_MAX;
    }
    else if (ma < INT16_MIN)
    {
        ma = INT16_MIN;
    }

    next.vbat_mv = (uint16_t)mv;
    next.ibat_ma = (int16_t)ma;
    next.vbus = (vbus.val1 & VBUS_PRESENT) ? true : next.vbus;
    state = decode_state(chg.val1, next.vbat_mv);
    next.soc_pct = soc_from_mv(next.vbat_mv);
    next.charge = charge_str(state);
    rising = vbat_is_rising(next.vbat_mv, next.vbus);
    next.aem = !next.vbus && (rising || (next.ibat_ma >= AEM_IBAT_CHG_MA));
    aem_active = next.aem;
    next.aem_window = aem_window_str(next.vbat_mv);
    next.vbat_rise_mv = vbat_rise_dv;
    next.source = source_str(next.vbus, next.usb_cc, charge_active(state), next.aem);

    publish(&next);

    LOG_INF("SRC:%s VBAT %u mV (%u%%) I %+d mA %s rise %+d/30s %s VBUS %s USB-CC %s",
            next.source,
            next.vbat_mv,
            next.soc_pct,
            next.ibat_ma,
            next.charge,
            next.vbat_rise_mv,
            next.aem_window,
            next.vbus ? "yes" : "no",
            next.usb_cc ? "yes" : "no");

    k_work_schedule_for_queue(&solar_wq, &sample_work, K_MSEC(STATUS_PERIOD_MS));
}

void solar_status_start(void)
{
    k_mutex_init(&snap_lock);
    k_work_queue_start(&solar_wq,
                       solar_stack,
                       K_KERNEL_STACK_SIZEOF(solar_stack),
                       SOLAR_PRIORITY,
                       NULL);
    k_work_init_delayable(&sample_work, sample_handler);
    k_work_schedule_for_queue(&solar_wq, &sample_work, K_NO_WAIT);
}

int solar_status_format(char *buf, size_t len)
{
    struct solar_snapshot copy;
    int wrote;

    if ((buf == NULL) || (len == 0U))
    {
        return -EINVAL;
    }

    k_mutex_lock(&snap_lock, K_FOREVER);
    copy = snap;
    k_mutex_unlock(&snap_lock);

    wrote = snprintk(buf,
                     len,
                     "{\"vbat_mv\":%u,\"soc_pct\":%u,\"ibat_ma\":%d,"
                     "\"charge\":\"%s\",\"vbus\":%s,\"usb_cc\":%s,"
                     "\"source\":\"%s\",\"aem\":%s,\"aem_window\":\"%s\","
                     "\"vbat_rise_mv\":%d}",
                     copy.vbat_mv,
                     copy.soc_pct,
                     copy.ibat_ma,
                     copy.charge,
                     copy.vbus ? "true" : "false",
                     copy.usb_cc ? "true" : "false",
                     copy.source,
                     copy.aem ? "true" : "false",
                     copy.aem_window,
                     copy.vbat_rise_mv);
    if ((wrote < 0) || ((size_t)wrote >= len))
    {
        return -ENOMEM;
    }
    return wrote;
}
