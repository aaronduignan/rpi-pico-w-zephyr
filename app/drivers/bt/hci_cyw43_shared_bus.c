/*
 * Bluetooth HCI driver for CYW43439 on Raspberry Pi Pico W.
 * Communicates via the shared PIO SPI bus alongside the AIROC WiFi driver.
 *
 * The CYW43439 has no dedicated BT UART on the Pico W — BT packets are
 * multiplexed over the same PIO SPI bus as WiFi using the cybt_shared_bus
 * protocol. This driver uses WHD's internal backplane access functions to
 * communicate with the BT core.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

/* WHD internal headers for backplane access */
#include "airoc_wifi.h"
#include "whd_int.h"
#include "bus_protocols/whd_bus_protocol_interface.h"

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT infineon_cyw43_bt_hci_shared_bus

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cyw43_bt_hci);

/* BT control register — used to verify BT core is accessible */
#define BT_CTRL_REG_ADDR  0x18000c7c

struct cyw43_bt_hci_data {
	bt_hci_recv_t recv;
	whd_driver_t whd_drv;
};

static whd_driver_t get_whd_driver(void)
{
	whd_interface_t iface = airoc_wifi_get_whd_interface();

	if (!iface) {
		return NULL;
	}

	/* whd_interface_t is whd_interface* and its first field is whd_driver_t */
	return ((struct whd_interface *)iface)->whd_driver;
}

static int cyw43_bt_open(const struct device *dev, bt_hci_recv_t recv)
{
	struct cyw43_bt_hci_data *data = dev->data;
	uint32_t bt_ctrl = 0;
	whd_result_t result;

	data->recv = recv;

	LOG_INF("CYW43 BT HCI open — probing chip via WHD backplane...");

	data->whd_drv = get_whd_driver();
	if (!data->whd_drv) {
		LOG_ERR("WiFi not initialised — cannot access shared bus");
		return -ENODEV;
	}

	/* Read BT_CTRL_REG to verify we can reach the BT core */
	result = whd_bus_read_backplane_value(data->whd_drv, BT_CTRL_REG_ADDR,
					      sizeof(bt_ctrl), (uint8_t *)&bt_ctrl);
	if (result != WHD_SUCCESS) {
		LOG_ERR("Backplane read failed (result %d)", result);
		return -EIO;
	}

	LOG_INF("BT_CTRL_REG = 0x%08x — BT core reachable", bt_ctrl);

	return -ENOSYS;
}

static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);

	net_buf_unref(buf);
	return -ENOSYS;
}

static DEVICE_API(bt_hci, cyw43_bt_hci_api) = {
	.open = cyw43_bt_open,
	.send = cyw43_bt_send,
};

#define CYW43_BT_HCI_INIT(inst)                                                \
	static struct cyw43_bt_hci_data cyw43_bt_data_##inst = {};             \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &cyw43_bt_data_##inst, NULL,   \
			      POST_KERNEL, CONFIG_BT_HCI_INIT_PRIORITY,        \
			      &cyw43_bt_hci_api);

DT_INST_FOREACH_STATUS_OKAY(CYW43_BT_HCI_INIT)
