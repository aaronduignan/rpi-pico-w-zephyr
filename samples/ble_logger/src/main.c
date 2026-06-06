#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_logger);

#ifndef CONFIG_BT_PERIPHERAL
#define CONFIG_BT_DEVICE_NAME_MAX 32
#endif

/* ── discovery table ──────────────────────────────────────────────────────── */

#define MAX_DEVICES 32

struct device_entry {
	bt_addr_le_t addr;
	int8_t       rssi;
	char         name[CONFIG_BT_DEVICE_NAME_MAX];
};

static struct device_entry g_devices[MAX_DEVICES];
static int                 g_device_count;
static K_MUTEX_DEFINE(g_devices_mutex);

/* ── state ────────────────────────────────────────────────────────────────── */

enum logger_state {
	STATE_IDLE,
	STATE_DISCOVERING,
	STATE_LOGGING,
};

static atomic_t g_state = ATOMIC_INIT(STATE_IDLE);

/* Signalled by cmd_scan on first use; main() blocks here until then so
 * bt_le_scan_start() runs on the main thread (not the shell thread) and
 * the radio is idle until the user actually asks for a scan. */
static K_SEM_DEFINE(g_start_scan_sem, 0, 1);

/* ── logging target ───────────────────────────────────────────────────────── */

static bt_addr_le_t g_target;
static bool         g_target_set;

/* ── scan parameters ──────────────────────────────────────────────────────── */

/*
 * Single passive continuous scan used for both discovery and logging.
 *
 * One set of params avoids any bt_le_scan_stop/start cycle when
 * transitioning between states.  The radio runs continuously from the first
 * 'ble scan' until the user calls 'ble stop' — state transitions are pure
 * software changes in the callback.
 *
 * Active scanning (CONFIG_BT_CENTRAL) would deliver device names via
 * SCAN_RSP but causes HCI LE Set Scan Enable timeouts on the CYW43439's
 * shared SPI bus and is not usable on this hardware.
 *
 * window == interval → continuous scan, radio never idle between windows.
 * Miss probability is channel mismatch only: each advertising event fires on
 * channels 37, 38, 39 in ~1–2 ms; the radio can only be on one at a time so
 * roughly 1-in-3 events will still be missed.
 *
 * BT_GAP_SCAN_FAST_INTERVAL = 0x0060 = 96 × 0.625 ms = 60 ms.
 */
static const struct bt_le_scan_param scan_param = {
	.type     = BT_LE_SCAN_TYPE_PASSIVE,
	.options  = BT_LE_SCAN_OPT_NONE,
	.interval = BT_GAP_SCAN_FAST_INTERVAL,  /* 60 ms */
	.window   = BT_GAP_SCAN_FAST_WINDOW,    /* 30 ms — 50% duty cycle, proven on CYW43439 */
};

/* ── helpers ──────────────────────────────────────────────────────────────── */

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

static bool log_ad_field_cb(struct bt_data *data, void *user_data)
{
	ARG_UNUSED(user_data);

	/* Up to 40 bytes as hex, truncated with " ..." if longer. */
	char hex[128];
	size_t len = MIN(data->data_len, 40U);
	size_t pos = 0;

	for (size_t i = 0; i < len; i++) {
		pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x", data->data[i]);
		if (i < len - 1) {
			hex[pos++] = ' ';
		}
	}
	if (data->data_len > 40) {
		snprintf(hex + pos, sizeof(hex) - pos, " ...");
	}

	LOG_INF("  [0x%02x] %s", data->type, hex);
	return true;
}

/* ── scan callback ────────────────────────────────────────────────────────── */

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	long s = atomic_get(&g_state);

	if (s == STATE_DISCOVERING) {
		struct net_buf_simple buf_copy;

		k_mutex_lock(&g_devices_mutex, K_FOREVER);

		for (int i = 0; i < g_device_count; i++) {
			if (memcmp(&g_devices[i].addr, addr, sizeof(*addr)) == 0) {
				k_mutex_unlock(&g_devices_mutex);
				return;
			}
		}

		if (g_device_count >= MAX_DEVICES) {
			k_mutex_unlock(&g_devices_mutex);
			return;
		}

		struct device_entry *e = &g_devices[g_device_count];

		memcpy(&e->addr, addr, sizeof(*addr));
		e->rssi = rssi;
		e->name[0] = '\0';
		net_buf_simple_clone(buf, &buf_copy);
		bt_data_parse(&buf_copy, parse_name_cb, e->name);

		int idx = ++g_device_count;

		k_mutex_unlock(&g_devices_mutex);

		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

		if (e->name[0] != '\0') {
			LOG_INF("[%2d] %s  RSSI %d dBm  \"%s\"",
				idx, addr_str, rssi, e->name);
		} else {
			LOG_INF("[%2d] %s  RSSI %d dBm",
				idx, addr_str, rssi);
		}

	} else if (s == STATE_LOGGING && g_target_set) {
		if (memcmp(addr, &g_target, sizeof(*addr)) != 0) {
			return;
		}

		uint32_t ms = k_uptime_get_32();
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

		LOG_INF("[%u:%02u:%02u.%03u] %s  RSSI %d dBm",
			ms / 3600000u,
			(ms % 3600000u) / 60000u,
			(ms % 60000u) / 1000u,
			ms % 1000u,
			addr_str, rssi);

		struct net_buf_simple buf_copy;

		net_buf_simple_clone(buf, &buf_copy);
		bt_data_parse(&buf_copy, log_ad_field_cb, NULL);
	}
}

/* ── discovery timeout ────────────────────────────────────────────────────── */

/*
 * Just change state — do NOT call bt_le_scan_stop() here.
 *
 * The CYW43439 shared SPI bus makes HCI command responses unreliable
 * mid-scan: a bt_le_scan_stop() from the workqueue races with WHD bus
 * activity and the HCI command complete event is lost.  The host then
 * times out and asserts.  Leaving the radio running and gating behaviour
 * through g_state is safe: the scan callback ignores packets in STATE_IDLE.
 * The only call to bt_le_scan_stop() is in cmd_stop, which is user-triggered
 * and therefore infrequent.
 */
static void discovery_end_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_set(&g_state, STATE_IDLE);

	k_mutex_lock(&g_devices_mutex, K_FOREVER);
	int count = g_device_count;

	k_mutex_unlock(&g_devices_mutex);

	LOG_INF("Discovery complete — %d device(s). Use 'ble log <N>' to start logging.", count);
}

static K_WORK_DELAYABLE_DEFINE(g_discovery_end_work, discovery_end_work_fn);

/* ── shell commands ───────────────────────────────────────────────────────── */

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_work_cancel_delayable(&g_discovery_end_work);

	k_mutex_lock(&g_devices_mutex, K_FOREVER);
	g_device_count = 0;
	k_mutex_unlock(&g_devices_mutex);

	atomic_set(&g_state, STATE_DISCOVERING);
	k_sem_give(&g_start_scan_sem);
	shell_print(sh, "Scanning for 10 s...");
	k_work_schedule(&g_discovery_end_work, K_SECONDS(10));
	return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: ble log <N>");
		return -EINVAL;
	}

	int n = atoi(argv[1]);

	k_work_cancel_delayable(&g_discovery_end_work);

	k_mutex_lock(&g_devices_mutex, K_FOREVER);

	if (n < 1 || n > g_device_count) {
		int count = g_device_count;

		k_mutex_unlock(&g_devices_mutex);
		shell_error(sh, "Invalid index (1-%d); run 'ble scan' first", count);
		return -EINVAL;
	}

	memcpy(&g_target, &g_devices[n - 1].addr, sizeof(g_target));
	k_mutex_unlock(&g_devices_mutex);

	g_target_set = true;
	atomic_set(&g_state, STATE_LOGGING);

	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&g_target, addr_str, sizeof(addr_str));
	shell_print(sh, "Logging %s — use 'ble stop' to end", addr_str);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_work_cancel_delayable(&g_discovery_end_work);
	atomic_set(&g_state, STATE_IDLE);
	g_target_set = false;
	/* bt_le_scan_stop() is intentionally not called — it reliably crashes on
	 * the CYW43439 shared SPI bus (HCI timeout or shell stack overflow).
	 * The radio keeps scanning; the callback ignores all packets in STATE_IDLE. */
	shell_print(sh, "Stopped");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ble_cmds,
	SHELL_CMD(scan, NULL,
		  "Discover BLE devices (10 s passive scan)", cmd_scan),
	SHELL_CMD_ARG(log, NULL,
		      "Log all advertisements from device N: log <N>", cmd_log, 2, 0),
	SHELL_CMD(stop, NULL,
		  "Stop scanning or logging", cmd_stop),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ble, &ble_cmds, "BLE logger", NULL);

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}

	LOG_INF("-----");
	LOG_INF("BLE logger ready — run 'ble scan' to discover devices");

	k_sem_take(&g_start_scan_sem, K_FOREVER);

	int scan_err = bt_le_scan_start(&scan_param, scan_cb);

	if (scan_err) {
		LOG_ERR("bt_le_scan_start failed: %d", scan_err);
	}

	return 0;
}
