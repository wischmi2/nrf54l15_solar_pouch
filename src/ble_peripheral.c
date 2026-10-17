/*
 * Copyright (c) 2026 Golioth
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(example_ble_peripheral);

#include "ble_peripheral.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>

#include <pouch/transport/bluetooth/gatt.h>

static struct bt_conn *default_conn;

static struct pouch_gatt_adv service_data = POUCH_GATT_ADV_DATA_INIT;

static struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_SVC_DATA16, &service_data, sizeof(service_data)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* 100–150 ms so a gateway finds the board quickly during the request window. */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
                                                                      BT_GAP_ADV_FAST_INT_MIN_2,
                                                                      BT_GAP_ADV_FAST_INT_MAX_2,
                                                                      NULL);

static bool sync_requested;

static void sync_window_stop(struct k_work *work);
static void sync_window_start(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(sync_window_stop_work, sync_window_stop);
K_WORK_DELAYABLE_DEFINE(sync_request_work, sync_window_start);

static void schedule_next_sync(void)
{
    (void) k_work_schedule(&sync_request_work, K_SECONDS(CONFIG_EXAMPLE_SYNC_PERIOD_S));
}

/* Advertise, with or without the flag the gateway uses to choose a device. */
static int advertise(bool request)
{
    int err;

    pouch_gatt_adv_req_sync(&service_data, request);

    err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err == -EALREADY)
    {
        err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    }
    if (err == 0)
    {
        sync_requested = request;
    }

    return err;
}

static void sync_window_stop(struct k_work *work)
{
    ARG_UNUSED(work);

    if (default_conn)
    {
        return;
    }

    int err = advertise(false);

    if (err)
    {
        LOG_ERR("Failed to clear gateway request (err %d)", err);
    }
    else
    {
        LOG_INF("Gateway request off for %d s", CONFIG_EXAMPLE_SYNC_PERIOD_S);
    }

    schedule_next_sync();
}

static void sync_window_start(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = advertise(true);

    if (err)
    {
        LOG_ERR("Failed to request a gateway (err %d)", err);
        schedule_next_sync();
        return;
    }

    LOG_INF("Requesting a gateway for %d s", CONFIG_EXAMPLE_ADV_WINDOW_S);
    (void) k_work_schedule(&sync_window_stop_work, K_SECONDS(CONFIG_EXAMPLE_ADV_WINDOW_S));
}

int ble_peripheral_start(void)
{
    sync_window_start(NULL);
    return sync_requested ? 0 : -EIO;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        return;
    }

    LOG_INF("Gateway connected");
    default_conn = conn;
    (void) k_work_cancel_delayable(&sync_window_stop_work);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    default_conn = NULL;
    (void) k_work_cancel_delayable(&sync_window_stop_work);

    int err = advertise(false);

    if (err)
    {
        LOG_ERR("Failed to resume advertising (err %d)", err);
    }

    LOG_INF("Disconnected (reason 0x%02x), next gateway request in %d s",
            reason,
            CONFIG_EXAMPLE_SYNC_PERIOD_S);
    schedule_next_sync();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];
    char passkey_str[7];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    (void) snprintf(passkey_str, sizeof(passkey_str), "%06u", passkey);

    LOG_INF("Passkey for %s: %s", addr, passkey_str);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];
    char passkey_str[7];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    (void) snprintf(passkey_str, sizeof(passkey_str), "%06u", passkey);

    LOG_INF("Confirm passkey for %s: %s", addr, passkey_str);

    if (IS_ENABLED(CONFIG_EXAMPLE_BT_AUTO_CONFIRM))
    {
        LOG_INF("Confirming passkey");
        bt_conn_auth_passkey_confirm(conn);
    }
}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing cancelled: %s", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .cancel = auth_cancel,
};

void ble_peripheral_request_gateway(bool request)
{
    pouch_gatt_adv_req_sync(&service_data, request);
    sync_requested = request;
    bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
}

bool ble_peripheral_sync_requested(void)
{
    return sync_requested;
}

void ble_peripheral_button_handler(void)
{
    if (default_conn)
    {
        LOG_INF("Confirming passkey");
        bt_conn_auth_passkey_confirm(default_conn);
    }
    else
    {
        LOG_WRN("No BT connection for passkey confirmation");
    }
}

int ble_peripheral_init(void)
{
    int err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    err = bt_conn_auth_cb_register(&auth_cb_display);
    if (err)
    {
        LOG_ERR("Bluetooth auth cb register failed (err %d)", err);
        return err;
    }

    return 0;
}
