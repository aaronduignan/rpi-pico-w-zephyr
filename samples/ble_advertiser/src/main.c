#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pico_app);

/*
 * Advertising interval — units are 0.625 ms; pick a min/max pair:
 *
 *   FAST_INT_MIN/MAX_1   30– 60 ms   (0x0030–0x0060)  high-rate scan window
 *   FAST_INT_MIN/MAX_2  100–150 ms   (0x00a0–0x00f0)  normal fast discovery
 *   SLOW_INT_MIN/MAX   1000–1200 ms  (0x0640–0x0780)  battery-friendly beacon
 *
 * Custom value: BT_GAP_ADV_INTERVAL_TO_US(n) converts n→µs for reference,
 * or just divide target-ms by 0.625 to get the raw value, e.g. 500 ms = 0x0320.
 *
 * options = 0 → non-connectable, non-scannable (phones see it but can't connect).
 */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
	0,
	BT_GAP_ADV_SLOW_INT_MIN,   /* 1000 ms */
	BT_GAP_ADV_SLOW_INT_MAX,   /* 1200 ms */
	NULL);

/*
 * Advertisement payload — total budget is 31 bytes across all fields.
 * Each field costs: 1 byte length + 1 byte type + N bytes data.
 *
 * Common field types (from assigned_numbers.h):
 *   BT_DATA_FLAGS             0x01  — always first; required for discoverability
 *   BT_DATA_NAME_COMPLETE     0x09  — full device name string
 *   BT_DATA_NAME_SHORTENED    0x08  — if name is too long to fit
 *   BT_DATA_UUID16_ALL        0x03  — 16-bit service UUIDs (e.g. 0x181A = env sensing)
 *   BT_DATA_SVC_DATA16        0x16  — 16-bit UUID + up to ~25 bytes of payload
 *   BT_DATA_MANUFACTURER_DATA 0xff  — 2-byte company ID + freeform payload
 *
 * BT_DATA_BYTES(type, byte, ...)  — inline literal bytes
 * BT_DATA(type, ptr, len)         — pointer to a buffer (e.g. updated at runtime)
 *
 * Example: manufacturer-specific field with a small sensor payload.
 * Company ID 0xFFFF is the test/unregistered ID — replace with your own
 * Bluetooth SIG allocation if you ever ship a product.
 */
static uint8_t mfr_payload[] = {
	0xff, 0xff,  /* company ID 0xFFFF (test) — little-endian */
	0x00,        /* app-defined version / frame type */
	0xBE,        /* payload byte 0 */
	0xEF,        /* payload byte 1 */
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_payload, sizeof(mfr_payload)),
};

int main(void)
{
	int err;

	LOG_INF("Pico W - Zephyr BLE beacon");

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	/*
	 * To update mfr_payload between intervals, write to the array and
	 * call bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0).
	 */
	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising start failed: %d", err);
		return err;
	}

	LOG_INF("Advertising as \"%s\" every ~1 s", CONFIG_BT_DEVICE_NAME);

	return 0;
}
