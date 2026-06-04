#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_scanner);

/* BT_DEVICE_NAME_MAX is only defined when CONFIG_BT_PERIPHERAL is enabled. */
#ifndef CONFIG_BT_PERIPHERAL
#define CONFIG_BT_DEVICE_NAME_MAX 32
#endif

/*
 * Passive scan parameters — we listen only, no scan request PDUs sent.
 *
 * Interval/window use the fast preset (60 ms / 30 ms). The window is how
 * long the radio listens per interval; a 50% duty cycle is a reasonable
 * balance between responsiveness and power.
 *
 * To change rate:
 *   BT_GAP_SCAN_FAST_INTERVAL / BT_GAP_SCAN_FAST_WINDOW   ~60 ms / 30 ms
 *   BT_GAP_SCAN_SLOW_INTERVAL_1 / BT_GAP_SCAN_SLOW_WINDOW_1  ~1.28 s / 11.25 ms
 */
static const struct bt_le_scan_param scan_param = {
	.type     = BT_LE_SCAN_TYPE_PASSIVE,
	.options  = BT_LE_SCAN_OPT_NONE,
	.interval = BT_GAP_SCAN_FAST_INTERVAL,
	.window   = BT_GAP_SCAN_FAST_WINDOW,
};

/*
 * Seen address table — each device is logged once; duplicates are silently
 * dropped. Sized for a busy environment; entries are never evicted
 * (reset the board to clear the list).
 */
#define MAX_SEEN 128

static bt_addr_le_t seen[MAX_SEEN];
static int seen_count;

static bool record_new(const bt_addr_le_t *addr)
{
	for (int i = 0; i < seen_count; i++) {
		if (memcmp(&seen[i], addr, sizeof(*addr)) == 0) {
			return false;
		}
	}

	if (seen_count < MAX_SEEN) {
		memcpy(&seen[seen_count++], addr, sizeof(*addr));
	}

	return true;
}

/* Parse the first complete or shortened name out of an AD payload. */
static bool parse_name_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE ||
	    data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, CONFIG_BT_DEVICE_NAME_MAX - 1);

		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	}

	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	char name[CONFIG_BT_DEVICE_NAME_MAX] = "";

	if (!record_new(addr)) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	bt_data_parse(buf, parse_name_cb, name);

	if (name[0] != '\0') {
		LOG_INF("[%3d] %s  RSSI %d dBm  \"%s\"",
			seen_count, addr_str, rssi, name);
	} else {
		LOG_INF("[%3d] %s  RSSI %d dBm",
			seen_count, addr_str, rssi);
	}
}

int main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	err = bt_le_scan_start(&scan_param, scan_cb);
	if (err) {
		LOG_ERR("Scan start failed: %d", err);
		return err;
	}

	LOG_INF("BLE scanner running — passive scan, new devices only");
	return 0;
}
