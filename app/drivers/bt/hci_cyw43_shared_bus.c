/*
 * Bluetooth HCI driver for CYW43439 on Raspberry Pi Pico W.
 * Communicates via the shared PIO SPI bus alongside the AIROC WiFi driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_cyw43_bt_hci_shared_bus

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cyw43_bt_hci);

struct cyw43_bt_hci_data {
	bt_hci_recv_t recv;
};

static int cyw43_bt_open(const struct device *dev, bt_hci_recv_t recv)
{
	struct cyw43_bt_hci_data *data = dev->data;

	data->recv = recv;

	LOG_INF("CYW43 BT HCI open - shared bus transport not yet implemented");

	return -ENOSYS;
}

static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);

	LOG_INF("CYW43 BT send - not yet implemented");
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
