#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zmk/ble.h>
#include <zmk/studio/core.h>
#include "viewer_ble.h"

#define VIEWER_UUID(n) BT_UUID_128_ENCODE(n, 0x0196, 0x6107, 0xc967, 0xc5cfb1c2482a)

K_MUTEX_DEFINE(connection_lock);
K_SEM_DEFINE(notification_done, 0, 1);
static struct bt_conn *viewer_conn;
static bool streaming;
static char pending[128];
static size_t pending_size;

static bool unlocked(void) {
    return zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    if (value != BT_GATT_CCC_NOTIFY) {
        k_mutex_lock(&connection_lock, K_FOREVER);
        streaming = false;
        k_mutex_unlock(&connection_lock);
    }
}

static ssize_t command_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset || len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (!unlocked() || bt_conn_get_security(conn) < BT_SECURITY_L2) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    }
    struct bt_conn *active = zmk_ble_active_profile_conn();
    bool permitted = active == conn;
    if (active) {
        bt_conn_unref(active);
    }
    if (!permitted) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    }
    uint8_t command = *(const uint8_t *)buf;
    if (command != 'C' && command != 'S' && command != 'E' && command != 'D') {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    k_mutex_lock(&connection_lock, K_FOREVER);
    if (viewer_conn != conn) {
        if (viewer_conn) {
            bt_conn_unref(viewer_conn);
        }
        viewer_conn = bt_conn_ref(conn);
        streaming = false;
    }
    if (command == 'E' || command == 'D') {
        streaming = command == 'E';
    }
    k_mutex_unlock(&connection_lock);
    if (command != 'D' && !viewer_ble_ready()) {
        k_mutex_lock(&connection_lock, K_FOREVER);
        streaming = false;
        k_mutex_unlock(&connection_lock);
        return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
    }
    if (command == 'E') {
        command = 'S';
    } else if (command == 'D') {
        return len;
    }
    if (viewer_request(command, true)) {
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(
    keyboard_viewer_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(VIEWER_UUID(0x00000010))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(VIEWER_UUID(0x00000011)),
                           BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT,
                           NULL, command_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(VIEWER_UUID(0x00000012)),
                           BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    k_mutex_lock(&connection_lock, K_FOREVER);
    if (conn == viewer_conn) {
        bt_conn_unref(viewer_conn);
        viewer_conn = NULL;
        streaming = false;
    }
    k_mutex_unlock(&connection_lock);
    k_sem_give(&notification_done);
}

BT_CONN_CB_DEFINE(viewer_connection_callbacks) = {.disconnected = disconnected};

static struct bt_conn *get_connection(void) {
    k_mutex_lock(&connection_lock, K_FOREVER);
    struct bt_conn *conn = viewer_conn ? bt_conn_ref(viewer_conn) : NULL;
    k_mutex_unlock(&connection_lock);
    struct bt_conn *active = zmk_ble_active_profile_conn();
    bool active_profile = conn && active == conn;
    if (active) {
        bt_conn_unref(active);
    }
    if (conn && (!active_profile || !unlocked() || bt_conn_get_security(conn) < BT_SECURITY_L2 ||
                 !bt_gatt_is_subscribed(conn, &keyboard_viewer_service.attrs[4],
                                        BT_GATT_CCC_NOTIFY))) {
        bt_conn_unref(conn);
        conn = NULL;
    }
    return conn;
}

bool viewer_ble_ready(void) {
    struct bt_conn *conn = get_connection();
    if (conn) {
        bt_conn_unref(conn);
    }
    return conn != NULL;
}

bool viewer_ble_streaming(void) {
    k_mutex_lock(&connection_lock, K_FOREVER);
    bool enabled = streaming;
    k_mutex_unlock(&connection_lock);
    return enabled && viewer_ble_ready();
}

static void notified(struct bt_conn *conn, void *user_data) {
    ARG_UNUSED(conn);
    ARG_UNUSED(user_data);
    k_sem_give(&notification_done);
}

static int flush_pending(void) {
    size_t offset = 0;
    while (offset < pending_size) {
        struct bt_conn *conn = get_connection();
        if (!conn) {
            pending_size = 0;
            return -ENOTCONN;
        }
        size_t chunk = MIN(pending_size - offset, bt_gatt_get_mtu(conn) - 3);
        k_sem_reset(&notification_done);
        struct bt_gatt_notify_params params = {
            .attr = &keyboard_viewer_service.attrs[4],
            .data = pending + offset,
            .len = chunk,
            .func = notified,
        };
        int err = bt_gatt_notify_cb(conn, &params);
        bt_conn_unref(conn);
        if (err || k_sem_take(&notification_done, K_MSEC(250))) {
            pending_size = 0;
            return err ? err : -ETIMEDOUT;
        }
        offset += chunk;
        /* One outstanding packet, and yield between chunks; never change HID connection params. */
        k_sleep(K_MSEC(3));
    }
    pending_size = 0;
    return 0;
}

int viewer_ble_send(const char *data, size_t size) {
    while (size) {
        size_t count = MIN(size, sizeof(pending) - pending_size);
        memcpy(pending + pending_size, data, count);
        pending_size += count;
        data += count;
        size -= count;
        if (pending_size == sizeof(pending)) {
            int err = flush_pending();
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

void viewer_ble_flush(void) {
    flush_pending();
}
