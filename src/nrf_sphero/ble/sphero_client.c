#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "sphero_client.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sphero_client, LOG_LEVEL_DBG);

enum {
    SPHERO_C_INITALIZED,
    SPHERO_C_NOTIF_ENABLED,
    SPHERO_C_WRITE_PENDING,
};

static uint8_t on_received(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data, uint16_t length)
{

    struct bt_sphero_client* sphero_c;

    // Retrieve Sphero Client module context
    sphero_c = CONTAINER_OF(params, struct bt_sphero_client, sphero_packet_subscribe_params);

    if (sphero_c->cb.received) {
        return sphero_c->cb.received(sphero_c, data, length, sphero_c->cb.recieved_context);
    }

    return BT_GATT_ITER_CONTINUE;
}

static void on_sent(struct bt_conn* conn, uint8_t err, struct bt_gatt_write_params* params)
{
    struct bt_sphero_client* sphero_c;
    const void* data;
    uint16_t length;

    /* Retrieve Sphero Client module context */
    sphero_c = CONTAINER_OF(params, struct bt_sphero_client, sphero_packet_write_params);

    /* Make a copy of the volatile data that is required by the callback */
    data = params->data;
    length = params->length;

    /* Clear the write pending flag */
    atomic_clear_bit(&sphero_c->state, SPHERO_C_WRITE_PENDING);

    /* Give the semaphore */
    k_sem_give(&sphero_c->sending);

    /* Run the callback */
    if (sphero_c->cb.sent) {
        sphero_c->cb.sent(sphero_c, err, data, length);
    }
}

int bt_sphero_client_init(struct bt_sphero_client* sphero_c, const struct bt_sphero_client_init_param* sphero_c_init)
{
    if (!sphero_c || !sphero_c_init) {
        return -EINVAL;
    }

    if (atomic_test_and_set_bit(&sphero_c->state, SPHERO_C_INITALIZED)) {
        return -EALREADY;
    }

    memcpy(&sphero_c->cb, &sphero_c_init->cb, sizeof(sphero_c->cb));

    k_sem_init(&sphero_c->sending, 1, 1);

    return 0;
}

int bt_sphero_client_send(struct bt_sphero_client* sphero_c, const uint8_t* data, uint16_t len)
{
    int err;

    // NOTE: THIS TIMEOUT SHOULD BE SHORTER
    err = k_sem_take(&sphero_c->sending, K_MSEC(400));

    if (err != 0) {
        LOG_ERR("Timeout while sending payload: Err %d", err);
        return err;
    }

    if (!sphero_c->conn) {
        return -ENOTCONN;
    }

    if (atomic_test_and_set_bit(&sphero_c->state, SPHERO_C_WRITE_PENDING)) {
        return -EALREADY;
    }

    sphero_c->sphero_packet_write_params.func = on_sent;
    sphero_c->sphero_packet_write_params.handle = sphero_c->handles.packets;
    sphero_c->sphero_packet_write_params.offset = 0;
    sphero_c->sphero_packet_write_params.data = data;
    sphero_c->sphero_packet_write_params.length = len;

    err = bt_gatt_write(sphero_c->conn, &sphero_c->sphero_packet_write_params);
    if (err) {
        atomic_clear_bit(&sphero_c->state, SPHERO_C_WRITE_PENDING);
    }

    return err;
}

int bt_sphero_handles_assign(struct bt_gatt_dm* dm, struct bt_sphero_client* sphero_c)
{
    const struct bt_gatt_dm_attr* gatt_service_attr = bt_gatt_dm_service_get(dm);
    const struct bt_gatt_service_val* gatt_service = bt_gatt_dm_attr_service_val(gatt_service_attr);
    const struct bt_gatt_dm_attr* gatt_chrc;
    const struct bt_gatt_dm_attr* gatt_desc;

    if (bt_uuid_cmp(gatt_service->uuid, BT_SPHERO_SERVICE_UUID)) {
        return -ENOTSUP;
    }

    LOG_DBG("Getting handles for Sphero Service");
    memset(&sphero_c->handles, 0xFF, sizeof(&sphero_c->handles));

    /* Sphero Packets characteristic */

    gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_SPHERO_PACKETS_UUID);
    if (!gatt_chrc) {
        LOG_ERR("Missing Sphero Packet characersitic");
        return -EINVAL;
    }

    /* Sphero Packets */
    gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_SPHERO_PACKETS_UUID);
    if (!gatt_desc) {
        LOG_ERR("Missing Sphero Packet value descriptor in characteristic");
        return -EINVAL;
    }

    LOG_DBG("Found handle for Sphero Packets characteristic");
    sphero_c->handles.packets = gatt_desc->handle;

    /* Sphero Packets CCC */
    gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_GATT_CCC);
    if (!gatt_desc) {
        LOG_ERR("Missing Sphero Packet CCC descriptor in characteristic");
        return -EINVAL;
    }

    LOG_DBG("Found handle for Sphero Packets CCC descriptor");
    sphero_c->handles.packets_ccc = gatt_desc->handle;

    /* Assign connection instance */
    sphero_c->conn = bt_gatt_dm_conn_get(dm);
    return 0;
}

int bt_sphero_subscribe(struct bt_sphero_client* sphero_c, bt_sphero_received_cb_t* received_cb, void* context)
{
    int err;

    if (atomic_test_and_set_bit(&sphero_c->state, SPHERO_C_NOTIF_ENABLED)) {
        return -EALREADY;
    }

    // Set the received callback

    sphero_c->cb.received = received_cb;
    sphero_c->cb.recieved_context = context;

    // Setup the subscribe params

    sphero_c->sphero_packet_subscribe_params.notify = on_received;
    sphero_c->sphero_packet_subscribe_params.value = BT_GATT_CCC_NOTIFY;
    sphero_c->sphero_packet_subscribe_params.value_handle = sphero_c->handles.packets;
    sphero_c->sphero_packet_subscribe_params.ccc_handle = sphero_c->handles.packets_ccc;
    atomic_set_bit(sphero_c->sphero_packet_subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    err = bt_gatt_subscribe(sphero_c->conn, &sphero_c->sphero_packet_subscribe_params);
    if (err) {
        LOG_ERR("Subscribe failed (err %d)", err);
        atomic_clear_bit(&sphero_c->state, SPHERO_C_NOTIF_ENABLED);
    } else {
        LOG_DBG("[SUBSCRIBED]");
    }

    return err;
}