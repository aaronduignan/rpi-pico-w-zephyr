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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <string.h>

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
#define HOST_CTRL_REG_ADDR 0x18000d6c
#define BT_BUF_REG_ADDR   0x18000c78
#define WLAN_RAM_BASE_REG_ADDR 0x18000d68

#define BTFW_MEM_OFFSET 0x19000000
#define BT2WLAN_PWRUP_ADDR 0x640894
#define BT2WLAN_PWRUP_WAKE 0x03

#define BTSDIO_FWBUF_SIZE 0x1000
#define BTSDIO_OFFSET_HOST_WRITE_BUF 0x0000
#define BTSDIO_OFFSET_HOST_READ_BUF BTSDIO_FWBUF_SIZE
#define BTSDIO_OFFSET_HOST2BT_IN 0x2000
#define BTSDIO_OFFSET_HOST2BT_OUT 0x2004
#define BTSDIO_OFFSET_BT2HOST_IN 0x2008
#define BTSDIO_OFFSET_BT2HOST_OUT 0x200c

#define BTSDIO_REG_DATA_VALID BIT(1)
#define BTSDIO_REG_WAKE_BT BIT(17)
#define BTSDIO_REG_SW_RDY BIT(24)
#define BTSDIO_REG_BT_AWAKE BIT(8)
#define BTSDIO_REG_FW_RDY BIT(24)

#define BTSDIO_FW_READY_RETRIES 300
#define BTSDIO_BT_AWAKE_RETRIES 300
#define BTSDIO_POLL_INTERVAL_MS 1
#define BTFW_WAIT_TIME_MS 150

#define BT_HCI_OP_READ_LOCAL_VERSION 0x1001
#define HCI_PACKET_TYPE_COMMAND 0x01
#define HCI_PACKET_TYPE_EVENT 0x04
#define HCI_EVENT_CMD_COMPLETE 0x0e

#define CYW43_BT_BUF_WORDS 80

#define BTFW_ADDR_MODE_EXTENDED 1
#define BTFW_ADDR_MODE_SEGMENT 2
#define BTFW_ADDR_MODE_LINEAR32 3

#define BTFW_HEX_LINE_TYPE_DATA 0
#define BTFW_HEX_LINE_TYPE_END_OF_DATA 1
#define BTFW_HEX_LINE_TYPE_EXTENDED_SEGMENT_ADDRESS 2
#define BTFW_HEX_LINE_TYPE_EXTENDED_ADDRESS 4
#define BTFW_HEX_LINE_TYPE_ABSOLUTE_32BIT_ADDRESS 5

extern const unsigned char cyw43_btfw_43439[];
extern const unsigned int cyw43_btfw_43439_len;

struct cyw43_bt_fw_buf {
	uint32_t h2b_buf;
	uint32_t b2h_buf;
	uint32_t h2b_in;
	uint32_t h2b_out;
	uint32_t b2h_in;
	uint32_t b2h_out;
};

struct cyw43_bt_fw_index {
	uint32_t h2b_in;
	uint32_t h2b_out;
	uint32_t b2h_in;
	uint32_t b2h_out;
};

struct cyw43_bt_hci_data {
	bt_hci_recv_t recv;
	whd_driver_t whd_drv;
	uint32_t host_ctrl_cache;
	struct cyw43_bt_fw_buf fw_buf;
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

static int cyw43_bt_read_reg(whd_driver_t whd_drv, const char *name,
			     uint32_t addr, uint32_t *value)
{
	whd_result_t result;

	*value = 0;
	result = whd_bus_read_backplane_value(whd_drv, addr, sizeof(*value),
					      (uint8_t *)value);
	if (result != WHD_SUCCESS) {
		LOG_ERR("%s read failed at 0x%08x (result %d)", name, addr,
			result);
		return -EIO;
	}

	LOG_INF("%s @ 0x%08x = 0x%08x", name, addr, *value);
	return 0;
}

static int cyw43_bt_reg_read(struct cyw43_bt_hci_data *data, uint32_t addr,
			     uint32_t *value)
{
	whd_result_t result;

	if (addr == HOST_CTRL_REG_ADDR) {
		*value = data->host_ctrl_cache;
		return 0;
	}

	result = whd_bus_read_backplane_value(data->whd_drv, addr, sizeof(*value),
					      (uint8_t *)value);
	return result == WHD_SUCCESS ? 0 : -EIO;
}

static int cyw43_bt_reg_write(struct cyw43_bt_hci_data *data, uint32_t addr,
			      uint32_t value)
{
	whd_result_t result;

	result = whd_bus_write_backplane_value(data->whd_drv, addr, sizeof(value),
					       value);
	if (result != WHD_SUCCESS) {
		return -EIO;
	}

	if (addr == HOST_CTRL_REG_ADDR) {
		data->host_ctrl_cache = value;
	}

	return 0;
}

static int cyw43_bt_mem_xfer(struct cyw43_bt_hci_data *data, bool write,
			     uint32_t addr, uint8_t *buf, uint32_t len)
{
	while (len > 0) {
		uint32_t chunk = MIN(len, 512);
		whd_result_t result;

		if (((addr & 0xfff) + chunk) > 0x1000) {
			chunk = 0x1000 - (addr & 0xfff);
		}

		result = whd_bus_mem_bytes(data->whd_drv,
					   write ? BUS_WRITE : BUS_READ, addr,
					   chunk, buf);
		if (result != WHD_SUCCESS) {
			LOG_ERR("BT memory %s failed at 0x%08x len %u (result %d)",
				write ? "write" : "read", addr, chunk, result);
			return -EIO;
		}

		addr += chunk;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int cyw43_bt_mem_read(struct cyw43_bt_hci_data *data, uint32_t addr,
			     void *buf, uint32_t len)
{
	return cyw43_bt_mem_xfer(data, false, addr, buf, len);
}

static int cyw43_bt_mem_write(struct cyw43_bt_hci_data *data, uint32_t addr,
			      const void *buf, uint32_t len)
{
	return cyw43_bt_mem_xfer(data, true, addr, (uint8_t *)buf, len);
}

static int cyw43_bt_write_firmware_record(struct cyw43_bt_hci_data *data,
					  uint32_t fw_addr,
					  const uint8_t *payload,
					  uint32_t payload_len)
{
	uint8_t aligned[CYW43_BT_BUF_WORDS * sizeof(uint32_t)] __aligned(4);
	uint8_t verify[CYW43_BT_BUF_WORDS * sizeof(uint32_t)] __aligned(4);
	uint32_t write_addr = BTFW_MEM_OFFSET + fw_addr;
	uint32_t aligned_addr = ROUND_DOWN(write_addr, 4);
	uint32_t offset = write_addr - aligned_addr;
	uint32_t write_len = ROUND_UP(offset + payload_len, 4);
	int ret;

	if (write_len > sizeof(aligned)) {
		LOG_ERR("BT firmware record too large (%u)", payload_len);
		return -EINVAL;
	}

	if (offset || write_len != payload_len) {
		ret = cyw43_bt_mem_read(data, aligned_addr, aligned, write_len);
		if (ret) {
			return ret;
		}
	}

	memcpy(&aligned[offset], payload, payload_len);

	ret = cyw43_bt_mem_write(data, aligned_addr, aligned, write_len);
	if (ret) {
		return ret;
	}

	ret = cyw43_bt_mem_read(data, aligned_addr, verify, write_len);
	if (ret) {
		LOG_ERR("BT firmware verify read failed at 0x%08x len %u",
			aligned_addr, write_len);
		return ret;
	}

	for (uint32_t i = 0; i < write_len; i++) {
		if (verify[i] != aligned[i]) {
			LOG_ERR("BT firmware verify mismatch at 0x%08x: wrote 0x%02x read 0x%02x",
				aligned_addr + i, aligned[i], verify[i]);
			return -EIO;
		}
	}

	return 0;
}

static int cyw43_bt_download_firmware(struct cyw43_bt_hci_data *data)
{
	const uint8_t *record = cyw43_btfw_43439;
	uint32_t remaining = cyw43_btfw_43439_len;
	uint8_t version_len;
	uint8_t record_count;
	uint16_t hi_addr = 0;
	uint32_t abs_base_addr = 0;
	int addr_mode = BTFW_ADDR_MODE_EXTENDED;
	uint32_t records = 0;
	int ret;

	if (cyw43_btfw_43439_len == 0) {
		LOG_ERR("BT firmware blob is empty");
		return -ENOENT;
	}

	version_len = *record++;
	remaining--;
	if (version_len > remaining) {
		LOG_ERR("BT firmware version string is truncated");
		return -EINVAL;
	}
	LOG_INF("BT firmware version: %s", (const char *)record);
	record += version_len;
	remaining -= version_len;

	if (remaining < 1) {
		LOG_ERR("BT firmware record count is missing");
		return -EINVAL;
	}
	record_count = *record++;
	remaining--;
	LOG_INF("BT firmware record count: %u", record_count);

	ret = cyw43_bt_reg_write(data, BTFW_MEM_OFFSET + BT2WLAN_PWRUP_ADDR,
				 BT2WLAN_PWRUP_WAKE);
	if (ret) {
		LOG_ERR("BT power-up/wake write failed");
		return ret;
	}

	while (remaining >= 4) {
		uint8_t data_len = record[0];
		uint16_t addr = ((uint16_t)record[1] << 8) | record[2];
		uint8_t type = record[3];
		const uint8_t *payload = &record[4];
		uint32_t dest_addr = addr;

		record += 4;
		remaining -= 4;

		if (data_len == 0 || type == BTFW_HEX_LINE_TYPE_END_OF_DATA) {
			break;
		}

		if (data_len > remaining) {
			LOG_ERR("Truncated BT firmware record at offset %u",
				cyw43_btfw_43439_len - remaining);
			return -EINVAL;
		}

		switch (type) {
		case BTFW_HEX_LINE_TYPE_EXTENDED_ADDRESS:
			if (data_len < 2) {
				return -EINVAL;
			}
			hi_addr = ((uint16_t)payload[0] << 8) | payload[1];
			addr_mode = BTFW_ADDR_MODE_EXTENDED;
			break;
		case BTFW_HEX_LINE_TYPE_EXTENDED_SEGMENT_ADDRESS:
			if (data_len < 2) {
				return -EINVAL;
			}
			hi_addr = ((uint16_t)payload[0] << 8) | payload[1];
			addr_mode = BTFW_ADDR_MODE_SEGMENT;
			break;
		case BTFW_HEX_LINE_TYPE_ABSOLUTE_32BIT_ADDRESS:
			if (data_len < 4) {
				return -EINVAL;
			}
			abs_base_addr = ((uint32_t)payload[0] << 24) |
					((uint32_t)payload[1] << 16) |
					((uint32_t)payload[2] << 8) |
					payload[3];
			addr_mode = BTFW_ADDR_MODE_LINEAR32;
			break;
		case BTFW_HEX_LINE_TYPE_DATA:
			if (addr_mode == BTFW_ADDR_MODE_EXTENDED) {
				dest_addr += (uint32_t)hi_addr << 16;
			} else if (addr_mode == BTFW_ADDR_MODE_SEGMENT) {
				dest_addr += (uint32_t)hi_addr << 4;
			} else if (addr_mode == BTFW_ADDR_MODE_LINEAR32) {
				dest_addr += abs_base_addr;
			}
			ret = cyw43_bt_write_firmware_record(data, dest_addr,
							    payload, data_len);
			if (ret) {
				return ret;
			}
			records++;
			break;
		default:
			LOG_ERR("Unexpected BT firmware record type 0x%02x", type);
			return -EINVAL;
		}

		record += data_len;
		remaining -= data_len;
	}

	LOG_INF("BT firmware copied via backplane (%u records, %u bytes)",
		records, cyw43_btfw_43439_len);
	return 0;
}

static bool cyw43_bt_fw_ready(struct cyw43_bt_hci_data *data)
{
	uint32_t value = 0;

	if (cyw43_bt_reg_read(data, BT_CTRL_REG_ADDR, &value)) {
		return false;
	}

	return (value & BTSDIO_REG_FW_RDY) != 0;
}

static bool cyw43_bt_awake(struct cyw43_bt_hci_data *data)
{
	uint32_t value = 0;

	if (cyw43_bt_reg_read(data, BT_CTRL_REG_ADDR, &value)) {
		return false;
	}

	return (value & BTSDIO_REG_BT_AWAKE) != 0;
}

static int cyw43_bt_wait_ready(struct cyw43_bt_hci_data *data)
{
	k_msleep(BTFW_WAIT_TIME_MS);

	for (int i = 0; i < BTSDIO_FW_READY_RETRIES; i++) {
		if (cyw43_bt_fw_ready(data)) {
			return 0;
		}
		k_msleep(BTSDIO_POLL_INTERVAL_MS);
	}

	return -ETIMEDOUT;
}

static int cyw43_bt_wait_awake(struct cyw43_bt_hci_data *data)
{
	for (int i = 0; i < BTSDIO_BT_AWAKE_RETRIES; i++) {
		if (cyw43_bt_awake(data)) {
			return 0;
		}
		k_msleep(BTSDIO_POLL_INTERVAL_MS);
	}

	return -ETIMEDOUT;
}

static int __maybe_unused cyw43_bt_set_bt_awake(struct cyw43_bt_hci_data *data,
					       bool awake)
{
	uint32_t value;
	int ret;

	ret = cyw43_bt_reg_read(data, HOST_CTRL_REG_ADDR, &value);
	if (ret) {
		return ret;
	}

	if (awake) {
		value |= BTSDIO_REG_WAKE_BT;
	} else {
		value &= ~BTSDIO_REG_WAKE_BT;
	}

	return cyw43_bt_reg_write(data, HOST_CTRL_REG_ADDR, value);
}

static int cyw43_bt_set_host_ready(struct cyw43_bt_hci_data *data)
{
	uint32_t value;
	int ret;

	ret = cyw43_bt_reg_read(data, HOST_CTRL_REG_ADDR, &value);
	if (ret) {
		return ret;
	}

	return cyw43_bt_reg_write(data, HOST_CTRL_REG_ADDR,
				  value | BTSDIO_REG_SW_RDY);
}

static int cyw43_bt_toggle_intr(struct cyw43_bt_hci_data *data)
{
	uint32_t value;
	int ret;

	ret = cyw43_bt_reg_read(data, HOST_CTRL_REG_ADDR, &value);
	if (ret) {
		return ret;
	}

	return cyw43_bt_reg_write(data, HOST_CTRL_REG_ADDR,
				  value ^ BTSDIO_REG_DATA_VALID);
}

static int cyw43_bt_init_buffers(struct cyw43_bt_hci_data *data)
{
	uint32_t base;
	uint32_t bt_buf;
	uint32_t bt_ctrl;
	uint32_t host_ctrl;
	uint32_t zero = 0;
	int ret;

	for (int i = 0; i < 300; i++) {
		ret = cyw43_bt_reg_read(data, WLAN_RAM_BASE_REG_ADDR, &base);
		if (ret) {
			return ret;
		}
		if (base != 0) {
			break;
		}
		k_msleep(1);
	}

	if (base == 0) {
		cyw43_bt_reg_read(data, BT_CTRL_REG_ADDR, &bt_ctrl);
		cyw43_bt_reg_read(data, HOST_CTRL_REG_ADDR, &host_ctrl);
		cyw43_bt_reg_read(data, BT_BUF_REG_ADDR, &bt_buf);
		LOG_ERR("BT post-ready regs: bt_ctrl=0x%08x host_ctrl=0x%08x bt_buf=0x%08x wlan_base=0x%08x",
			bt_ctrl, host_ctrl, bt_buf, base);
		LOG_ERR("BT firmware did not publish WLAN RAM base");
		return -EIO;
	}

	data->fw_buf.h2b_buf = base + BTSDIO_OFFSET_HOST_WRITE_BUF;
	data->fw_buf.b2h_buf = base + BTSDIO_OFFSET_HOST_READ_BUF;
	data->fw_buf.h2b_in = base + BTSDIO_OFFSET_HOST2BT_IN;
	data->fw_buf.h2b_out = base + BTSDIO_OFFSET_HOST2BT_OUT;
	data->fw_buf.b2h_in = base + BTSDIO_OFFSET_BT2HOST_IN;
	data->fw_buf.b2h_out = base + BTSDIO_OFFSET_BT2HOST_OUT;

	LOG_INF("BT ring base=0x%08x h2b=0x%08x b2h=0x%08x",
		base, data->fw_buf.h2b_buf, data->fw_buf.b2h_buf);

	ret = cyw43_bt_reg_write(data, data->fw_buf.h2b_in, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.h2b_out, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.b2h_in, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.b2h_out, zero);

	return ret ? -EIO : 0;
}

static int cyw43_bt_read_indices(struct cyw43_bt_hci_data *data,
				 struct cyw43_bt_fw_index *index)
{
	uint32_t values[4] __aligned(4);
	int ret;

	ret = cyw43_bt_mem_read(data, data->fw_buf.h2b_in, values,
				sizeof(values));
	if (ret) {
		return ret;
	}

	index->h2b_in = values[0];
	index->h2b_out = values[1];
	index->b2h_in = values[2];
	index->b2h_out = values[3];

	return 0;
}

static uint32_t cyw43_bt_circ_count(uint32_t in, uint32_t out)
{
	return (in - out) & (BTSDIO_FWBUF_SIZE - 1);
}

static uint32_t cyw43_bt_circ_space(uint32_t in, uint32_t out)
{
	return cyw43_bt_circ_count(out, in + 4);
}

static int cyw43_bt_ring_write(struct cyw43_bt_hci_data *data,
			       const uint8_t *packet, uint32_t len)
{
	uint8_t tx[CYW43_BT_BUF_WORDS * sizeof(uint32_t)] __aligned(4);
	struct cyw43_bt_fw_index index;
	uint32_t rounded_len = ROUND_UP(len, 4);
	uint32_t new_in;
	int ret;

	if (rounded_len > sizeof(tx)) {
		return -EINVAL;
	}

	memset(tx, 0, rounded_len);
	memcpy(tx, packet, len);

	ret = cyw43_bt_read_indices(data, &index);
	if (ret) {
		return ret;
	}

	LOG_INF("BT ring before write: h2b_in=%u h2b_out=%u b2h_in=%u b2h_out=%u",
		index.h2b_in, index.h2b_out, index.b2h_in, index.b2h_out);

	if (rounded_len > cyw43_bt_circ_space(index.h2b_in, index.h2b_out)) {
		return -ENOBUFS;
	}

	if (index.h2b_in + rounded_len <= BTSDIO_FWBUF_SIZE) {
		ret = cyw43_bt_mem_write(data, data->fw_buf.h2b_buf + index.h2b_in,
					 tx, rounded_len);
		new_in = index.h2b_in + rounded_len;
	} else {
		uint32_t first = BTSDIO_FWBUF_SIZE - index.h2b_in;

		ret = cyw43_bt_mem_write(data, data->fw_buf.h2b_buf + index.h2b_in,
					 tx, first);
		if (!ret) {
			ret = cyw43_bt_mem_write(data, data->fw_buf.h2b_buf,
						 tx + first, rounded_len - first);
		}
		new_in = rounded_len - first;
	}
	if (ret) {
		return ret;
	}

	ret = cyw43_bt_reg_write(data, data->fw_buf.h2b_in,
				 new_in & (BTSDIO_FWBUF_SIZE - 1));
	if (ret) {
		return ret;
	}

	return cyw43_bt_toggle_intr(data);
}

static int cyw43_bt_ring_read(struct cyw43_bt_hci_data *data, uint8_t *packet,
			      uint32_t max_len, uint32_t *read_len)
{
	struct cyw43_bt_fw_index index;
	uint8_t header[4] __aligned(4);
	uint32_t available;
	uint32_t payload_len;
	uint32_t total_len;
	uint32_t rounded_len;
	uint32_t out;
	int ret;

	*read_len = 0;

	ret = cyw43_bt_read_indices(data, &index);
	if (ret) {
		return ret;
	}

	available = cyw43_bt_circ_count(index.b2h_in, index.b2h_out);
	if (available < sizeof(header)) {
		return -EAGAIN;
	}

	ret = cyw43_bt_mem_read(data, data->fw_buf.b2h_buf + index.b2h_out,
				header, sizeof(header));
	if (ret) {
		return ret;
	}

	payload_len = sys_get_le24(header);
	total_len = payload_len + sizeof(header);
	rounded_len = ROUND_UP(total_len, 4);
	if (total_len > max_len) {
		return -ENOBUFS;
	}
	if (available < rounded_len) {
		return -EAGAIN;
	}

	if (index.b2h_out + rounded_len <= BTSDIO_FWBUF_SIZE) {
		ret = cyw43_bt_mem_read(data, data->fw_buf.b2h_buf + index.b2h_out,
					packet, rounded_len);
		out = index.b2h_out + rounded_len;
	} else {
		uint32_t first = BTSDIO_FWBUF_SIZE - index.b2h_out;

		ret = cyw43_bt_mem_read(data, data->fw_buf.b2h_buf + index.b2h_out,
					packet, first);
		if (!ret) {
			ret = cyw43_bt_mem_read(data, data->fw_buf.b2h_buf,
						packet + first, rounded_len - first);
		}
		out = rounded_len - first;
	}
	if (ret) {
		return ret;
	}

	ret = cyw43_bt_reg_write(data, data->fw_buf.b2h_out,
				 out & (BTSDIO_FWBUF_SIZE - 1));
	if (ret) {
		return ret;
	}

	*read_len = total_len;
	return cyw43_bt_toggle_intr(data);
}

static int cyw43_bt_probe_version(struct cyw43_bt_hci_data *data)
{
	uint8_t cmd[8] __aligned(4) = {
		0x03, 0x00, 0x00, HCI_PACKET_TYPE_COMMAND,
		BT_HCI_OP_READ_LOCAL_VERSION & 0xff,
		BT_HCI_OP_READ_LOCAL_VERSION >> 8,
		0x00,
	};
	uint8_t event[80] __aligned(4);
	uint32_t event_len;
	int ret;

	ret = cyw43_bt_ring_write(data, cmd, 7);
	if (ret) {
		LOG_ERR("BT version command write failed (%d)", ret);
		return ret;
	}

	for (int i = 0; i < 300; i++) {
		ret = cyw43_bt_ring_read(data, event, sizeof(event), &event_len);
		if (ret == -EAGAIN) {
			k_msleep(1);
			continue;
		}
		if (ret) {
			LOG_ERR("BT version event read failed (%d)", ret);
			return ret;
		}

		LOG_INF("BT event: type=0x%02x event=0x%02x len=%u",
			event[3], event[4], event_len);

		if (event_len >= 18 && event[3] == HCI_PACKET_TYPE_EVENT &&
		    event[4] == HCI_EVENT_CMD_COMPLETE &&
		    sys_get_le16(&event[7]) == BT_HCI_OP_READ_LOCAL_VERSION) {
			LOG_INF("BT local version: status=0x%02x hci=0x%02x rev=0x%04x lmp=0x%02x manufacturer=0x%04x subver=0x%04x",
				event[9], event[10], sys_get_le16(&event[11]),
				event[13], sys_get_le16(&event[14]),
				sys_get_le16(&event[16]));
			return 0;
		}
	}

	LOG_ERR("BT version command timed out");
	return -ETIMEDOUT;
}

static int cyw43_bt_open(const struct device *dev, bt_hci_recv_t recv)
{
	struct cyw43_bt_hci_data *data = dev->data;
	struct whd_bt_info bt_info;
	whd_driver_t shared_drv;
	uint32_t reg_value;
	whd_result_t result;
	int ret;

	data->recv = recv;

	LOG_INF("HCI open — probing chip via WHD backplane...");

	data->whd_drv = get_whd_driver();
	if (!data->whd_drv) {
		LOG_ERR("WiFi not initialised — cannot access shared bus");
		return -ENODEV;
	}

	result = whd_bus_share_bt_init(data->whd_drv);
	if (result != WHD_SUCCESS) {
		LOG_ERR("WHD shared BT bus init failed (result %d)", result);
		return -EIO;
	}
	LOG_INF("WHD shared BT bus init OK");

	shared_drv = whd_bt_get_whd_driver();
	LOG_INF("WHD BT driver handle %p (WiFi handle %p)",
		(void *)shared_drv, (void *)data->whd_drv);
	if (shared_drv != data->whd_drv) {
		LOG_ERR("WHD BT driver handle mismatch");
		return -EIO;
	}

	result = whd_get_bt_info(data->whd_drv, &bt_info);
	if (result != WHD_SUCCESS) {
		LOG_ERR("WHD BT info failed (result %d)", result);
		return -EIO;
	}
	LOG_INF("WHD BT info: bt_ctrl=0x%08x host_ctrl=0x%08x bt_buf=0x%08x wlan_buf=0x%08x",
		bt_info.bt_ctrl_reg_addr, bt_info.host_ctrl_reg_addr,
		bt_info.bt_buf_reg_addr, bt_info.wlan_buf_addr);

	ret = cyw43_bt_read_reg(data->whd_drv, "BT_CTRL_REG",
				bt_info.bt_ctrl_reg_addr ?
				bt_info.bt_ctrl_reg_addr : BT_CTRL_REG_ADDR,
				&reg_value);
	if (ret) {
		return ret;
	}

	ret = cyw43_bt_read_reg(data->whd_drv, "BT_BUF_REG",
				bt_info.bt_buf_reg_addr ?
				bt_info.bt_buf_reg_addr : BT_BUF_REG_ADDR,
				&reg_value);
	if (ret) {
		return ret;
	}

	ret = cyw43_bt_read_reg(data->whd_drv, "HOST_CTRL_REG",
				bt_info.host_ctrl_reg_addr ?
				bt_info.host_ctrl_reg_addr : HOST_CTRL_REG_ADDR,
				&reg_value);
	if (ret) {
		return ret;
	}

	result = whd_bus_bt_attach(data->whd_drv, data, NULL);
	if (result != WHD_SUCCESS) {
		LOG_ERR("WHD BT attach failed (result %d)", result);
		return -EIO;
	}
	LOG_INF("WHD BT attach OK — downloading BT firmware");

	ret = cyw43_bt_download_firmware(data);
	if (ret) {
		LOG_ERR("BT firmware download failed (%d)", ret);
		return ret;
	}

	ret = cyw43_bt_wait_ready(data);
	if (ret) {
		LOG_ERR("BT firmware ready wait failed (%d)", ret);
		cyw43_bt_read_reg(data->whd_drv, "BT_CTRL_REG", BT_CTRL_REG_ADDR,
				  &reg_value);
		cyw43_bt_read_reg(data->whd_drv, "WLAN_RAM_BASE_REG",
				  WLAN_RAM_BASE_REG_ADDR, &reg_value);
		return ret;
	}
	LOG_INF("BT firmware ready");

	ret = cyw43_bt_init_buffers(data);
	if (ret) {
		LOG_ERR("BT buffer init failed (%d)", ret);
		return ret;
	}

	ret = cyw43_bt_wait_awake(data);
	if (ret) {
		LOG_ERR("BT awake wait failed (%d)", ret);
		return ret;
	}

	ret = cyw43_bt_set_host_ready(data);
	if (ret) {
		LOG_ERR("BT host-ready write failed (%d)", ret);
		return ret;
	}

	ret = cyw43_bt_toggle_intr(data);
	if (ret) {
		LOG_ERR("BT host-ready interrupt failed (%d)", ret);
		return ret;
	}

	ret = cyw43_bt_probe_version(data);
	if (ret) {
		LOG_ERR("BT version probe failed (%d)", ret);
		return ret;
	}

	LOG_INF("BT shared-bus probe complete — Zephyr HCI transport not implemented yet");

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
