/*
 * Bluetooth HCI driver for CYW43439 on Raspberry Pi Pico W.
 * Communicates via the shared PIO SPI bus alongside the AIROC WiFi driver.
 *
 * The CYW43439 has no dedicated BT UART on the Pico W — BT packets are
 * multiplexed over the same PIO SPI bus as WiFi using the BTSDIO protocol.
 * This driver uses WHD's internal backplane access functions to communicate
 * with the BT core, and implements Zephyr's bt_hci_driver_api (open + send).
 *
 * Packet flow:
 *   TX: Zephyr BT stack → cyw43_bt_send() → H2B ring → CYW43 BT core
 *   RX: CYW43 BT core → B2H ring → cyw43_bt_rx_thread() → bt_hci_recv()
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
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

/* Backplane register addresses for BT/host handshake */
#define BT_CTRL_REG_ADDR       0x18000c7c
#define HOST_CTRL_REG_ADDR     0x18000d6c
#define BT_BUF_REG_ADDR        0x18000c78
#define WLAN_RAM_BASE_REG_ADDR 0x18000d68

/* Firmware load base and BT power-up sequence */
#define BTFW_MEM_OFFSET    0x19000000
#define BT2WLAN_PWRUP_ADDR 0x640894
#define BT2WLAN_PWRUP_WAKE 0x03

/*
 * BTSDIO ring buffer layout (relative to WLAN RAM base published by firmware):
 *   0x0000–0x0FFF  H2B data buffer (host-to-BT, 4 KB)
 *   0x1000–0x1FFF  B2H data buffer (BT-to-host, 4 KB)
 *   0x2000         h2b_in  (host write pointer)
 *   0x2004         h2b_out (BT read pointer)
 *   0x2008         b2h_in  (BT write pointer)
 *   0x200c         b2h_out (host read pointer)
 */
#define BTSDIO_FWBUF_SIZE            0x1000
#define BTSDIO_OFFSET_HOST_WRITE_BUF 0x0000
#define BTSDIO_OFFSET_HOST_READ_BUF  BTSDIO_FWBUF_SIZE
#define BTSDIO_OFFSET_HOST2BT_IN     0x2000
#define BTSDIO_OFFSET_HOST2BT_OUT    0x2004
#define BTSDIO_OFFSET_BT2HOST_IN     0x2008
#define BTSDIO_OFFSET_BT2HOST_OUT    0x200c

/* HOST_CTRL_REG bit fields */
#define BTSDIO_REG_DATA_VALID BIT(1)   /* toggled to signal data available */
#define BTSDIO_REG_WAKE_BT    BIT(17)  /* assert to wake BT core from sleep */
#define BTSDIO_REG_SW_RDY     BIT(24)  /* set once host is ready for traffic */

/* BT_CTRL_REG bit fields (read-only, written by firmware) */
#define BTSDIO_REG_BT_AWAKE BIT(8)   /* BT core is awake and ready */
#define BTSDIO_REG_FW_RDY   BIT(24)  /* firmware boot complete */

#define BTSDIO_FW_READY_RETRIES  300
#define BTSDIO_BT_AWAKE_RETRIES  300
#define BTSDIO_POLL_INTERVAL_MS  1
#define BTFW_WAIT_TIME_MS        150
#define BTFW_PWRUP_DELAY_MS      2    /* settle time after BT2WLAN_PWRUP_WAKE */
#define BTFW_WRITE_RETRIES       3    /* retries on backplane verify mismatch */

/* H:4 UART transport packet type bytes */
#define HCI_PACKET_TYPE_COMMAND 0x01
#define HCI_PACKET_TYPE_ACL     0x02
#define HCI_PACKET_TYPE_EVENT   0x04

#define CYW43_BT_RX_STACK_SIZE 4096
#define CYW43_BT_RX_PRIORITY   K_PRIO_PREEMPT(5)
#define CYW43_BT_RX_BUF_SIZE   264  /* 4-byte BTSDIO header + 260 HCI payload */
#define CYW43_BT_BUF_WORDS     80   /* max firmware record size in 32-bit words */

/* Intel HEX address modes used in the firmware blob */
#define BTFW_ADDR_MODE_EXTENDED 1
#define BTFW_ADDR_MODE_SEGMENT  2
#define BTFW_ADDR_MODE_LINEAR32 3

/* Intel HEX record types present in the firmware blob */
#define BTFW_HEX_LINE_TYPE_DATA                    0
#define BTFW_HEX_LINE_TYPE_END_OF_DATA             1
#define BTFW_HEX_LINE_TYPE_EXTENDED_SEGMENT_ADDRESS 2
#define BTFW_HEX_LINE_TYPE_EXTENDED_ADDRESS        4
#define BTFW_HEX_LINE_TYPE_ABSOLUTE_32BIT_ADDRESS  5

extern const unsigned char cyw43_btfw_43439[];
extern const unsigned int cyw43_btfw_43439_len;

/**
 * Ring buffer register addresses, resolved after firmware publishes the
 * WLAN RAM base. Stored as absolute backplane addresses ready for mem_read
 * and reg_write calls.
 */
struct cyw43_bt_fw_buf {
	uint32_t h2b_buf;  /* base address of H2B data ring */
	uint32_t b2h_buf;  /* base address of B2H data ring */
	uint32_t h2b_in;   /* address of h2b_in  index register */
	uint32_t h2b_out;  /* address of h2b_out index register */
	uint32_t b2h_in;   /* address of b2h_in  index register */
	uint32_t b2h_out;  /* address of b2h_out index register */
};

/**
 * Snapshot of all four ring indices read in one backplane transaction.
 * h2b_in/h2b_out are owned by host/BT respectively for the H2B direction,
 * and reversed for B2H.
 */
struct cyw43_bt_fw_index {
	uint32_t h2b_in;
	uint32_t h2b_out;
	uint32_t b2h_in;
	uint32_t b2h_out;
};

/**
 * Per-device instance data for the HCI driver.
 *
 * host_ctrl_cache shadows HOST_CTRL_REG to avoid a bus read on every
 * toggle — HOST_CTRL_REG is write-only in practice, so we maintain the
 * last written value locally.
 */
struct cyw43_bt_hci_data {
	bt_hci_recv_t recv;
	whd_driver_t whd_drv;
	uint32_t host_ctrl_cache;
	struct cyw43_bt_fw_buf fw_buf;
	struct k_thread rx_thread;
};

K_THREAD_STACK_DEFINE(cyw43_bt_rx_stack, CYW43_BT_RX_STACK_SIZE);

/**
 * Retrieve the WHD driver handle from the AIROC WiFi interface.
 * WiFi must be fully initialised before this is called; the BT shared bus
 * sits on top of the same WHD instance.
 *
 * @return WHD driver handle, or NULL if WiFi is not yet initialised.
 */
static whd_driver_t get_whd_driver(void)
{
	whd_interface_t iface = airoc_wifi_get_whd_interface();

	if (!iface) {
		return NULL;
	}

	/* whd_interface_t is whd_interface* and its first field is whd_driver_t */
	return ((struct whd_interface *)iface)->whd_driver;
}

/**
 * Read a backplane register and log its value. Used only during init to
 * verify the chip is accessible and print diagnostic register state.
 *
 * @param whd_drv  WHD driver handle.
 * @param name     Human-readable register name for log output.
 * @param addr     Backplane address to read.
 * @param value    Output: register value on success, 0 on error.
 * @return 0 on success, -EIO on bus failure.
 */
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

/**
 * Read a 32-bit backplane register.
 * HOST_CTRL_REG reads are served from the local cache to avoid a bus
 * round-trip — the register is effectively write-only from the host side.
 *
 * @param data  Driver instance data.
 * @param addr  Backplane address.
 * @param value Output: register value.
 * @return 0 on success, -EIO on bus failure.
 */
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

/**
 * Write a 32-bit backplane register.
 * HOST_CTRL_REG writes are mirrored into host_ctrl_cache so that
 * subsequent read-modify-write operations on that register don't need a
 * bus read.
 *
 * @param data  Driver instance data.
 * @param addr  Backplane address.
 * @param value Value to write.
 * @return 0 on success, -EIO on bus failure.
 */
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

/**
 * Transfer an arbitrary-length block to or from the BT backplane.
 * The WHD backplane window is 4 KB; transfers are split at window
 * boundaries to avoid crossing them in a single call.
 *
 * @param data   Driver instance data.
 * @param write  true for host→chip, false for chip→host.
 * @param addr   Starting backplane address.
 * @param buf    Data buffer (read into or written from).
 * @param len    Number of bytes to transfer.
 * @return 0 on success, -EIO on bus failure.
 */
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

/** Read @p len bytes from backplane @p addr into @p buf. */
static int cyw43_bt_mem_read(struct cyw43_bt_hci_data *data, uint32_t addr,
			     void *buf, uint32_t len)
{
	return cyw43_bt_mem_xfer(data, false, addr, buf, len);
}

/** Write @p len bytes from @p buf to backplane @p addr. */
static int cyw43_bt_mem_write(struct cyw43_bt_hci_data *data, uint32_t addr,
			      const void *buf, uint32_t len)
{
	return cyw43_bt_mem_xfer(data, true, addr, (uint8_t *)buf, len);
}

/**
 * Write one Intel HEX data record to the BT core via backplane.
 *
 * The backplane requires 4-byte-aligned accesses. If fw_addr is
 * unaligned, a read-modify-write is performed around the payload.
 *
 * Each write is verified by reading back and comparing. The CYW43439
 * shares its backplane with the WiFi core over a single PIO SPI bus —
 * there is no bus arbitration between the two cores, so writes can
 * silently fail if the BT core hasn't fully woken after the PWRUP signal.
 * BTFW_WRITE_RETRIES retries with a 1 ms delay between attempts handle
 * this without propagating a spurious error to the caller.
 *
 * @param data        Driver instance data.
 * @param fw_addr     Destination address within firmware memory space.
 * @param payload     Record payload bytes.
 * @param payload_len Payload length in bytes.
 * @return 0 on success, -EIO if all retry attempts fail, negative errno on bus error.
 */
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

	for (int attempt = 0; attempt < BTFW_WRITE_RETRIES; attempt++) {
		if (attempt > 0) {
			LOG_WRN("BT firmware write retry %d at 0x%08x",
				attempt, aligned_addr);
			k_msleep(1);
		}

		ret = cyw43_bt_mem_write(data, aligned_addr, aligned, write_len);
		if (ret) {
			continue;
		}

		ret = cyw43_bt_mem_read(data, aligned_addr, verify, write_len);
		if (ret) {
			continue;
		}

		bool match = true;

		for (uint32_t i = 0; i < write_len; i++) {
			if (verify[i] != aligned[i]) {
				match = false;
				break;
			}
		}

		if (match) {
			return 0;
		}
	}

	LOG_ERR("BT firmware write failed after %d attempts at 0x%08x",
		BTFW_WRITE_RETRIES, aligned_addr);
	return -EIO;
}

/**
 * Parse and download the BT firmware blob to the CYW43439 BT core.
 *
 * The blob is a binary Intel HEX stream prefixed with a version string and
 * a record count. Address records (extended, segment, linear32) set the
 * base for subsequent data records; only data records are written and
 * counted. Address records are not counted in the logged record total.
 *
 * @param data  Driver instance data.
 * @return 0 on success, negative errno on parse or write error.
 */
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
	k_msleep(BTFW_PWRUP_DELAY_MS);

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

/**
 * @return true if BT_CTRL_REG FW_RDY bit is set (firmware boot complete).
 */
static bool cyw43_bt_fw_ready(struct cyw43_bt_hci_data *data)
{
	uint32_t value = 0;

	if (cyw43_bt_reg_read(data, BT_CTRL_REG_ADDR, &value)) {
		return false;
	}

	return (value & BTSDIO_REG_FW_RDY) != 0;
}

/**
 * @return true if BT_CTRL_REG BT_AWAKE bit is set (BT core is awake).
 */
static bool cyw43_bt_awake(struct cyw43_bt_hci_data *data)
{
	uint32_t value = 0;

	if (cyw43_bt_reg_read(data, BT_CTRL_REG_ADDR, &value)) {
		return false;
	}

	return (value & BTSDIO_REG_BT_AWAKE) != 0;
}

/**
 * Block until FW_RDY is set, with an initial 150 ms delay to allow the
 * firmware to start executing after download.
 *
 * @return 0 when ready, -ETIMEDOUT if FW_RDY never asserts.
 */
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

/**
 * Block until BT_AWAKE is set. Called before every send to ensure the
 * BT core has come out of sleep before the H2B ring is written.
 *
 * @return 0 when awake, -ETIMEDOUT if BT_AWAKE never asserts.
 */
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

/**
 * Assert or deassert the WAKE_BT bit in HOST_CTRL_REG to request the BT
 * core wake from sleep before a host-to-BT transfer.
 *
 * @param data   Driver instance data.
 * @param awake  true to assert WAKE_BT, false to deassert.
 * @return 0 on success, negative errno on bus error.
 */
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

/**
 * Set SW_RDY in HOST_CTRL_REG to signal the host side is initialised and
 * ready to exchange HCI traffic. Called once during open, after ring buffer
 * setup is complete.
 *
 * @return 0 on success, negative errno on bus error.
 */
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

/**
 * Toggle the DATA_VALID bit in HOST_CTRL_REG to notify the BT core that
 * the ring buffer state has changed (data written or consumed).
 *
 * The BTSDIO protocol uses edge signalling — the BT core detects the bit
 * change, not a specific level — so the bit is XOR'd rather than set.
 *
 * @return 0 on success, negative errno on bus error.
 */
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

/**
 * Wait for the BT firmware to publish the WLAN RAM base address, then
 * resolve all ring buffer register addresses and zero the four indices.
 *
 * The firmware writes its WLAN RAM base to WLAN_RAM_BASE_REG after boot;
 * the ring buffer layout is fixed relative to that base.
 *
 * @return 0 on success, -EIO if the base address is never published.
 */
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
	data->fw_buf.h2b_in  = base + BTSDIO_OFFSET_HOST2BT_IN;
	data->fw_buf.h2b_out = base + BTSDIO_OFFSET_HOST2BT_OUT;
	data->fw_buf.b2h_in  = base + BTSDIO_OFFSET_BT2HOST_IN;
	data->fw_buf.b2h_out = base + BTSDIO_OFFSET_BT2HOST_OUT;

	LOG_INF("BT ring base=0x%08x h2b=0x%08x b2h=0x%08x",
		base, data->fw_buf.h2b_buf, data->fw_buf.b2h_buf);

	ret = cyw43_bt_reg_write(data, data->fw_buf.h2b_in, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.h2b_out, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.b2h_in, zero);
	ret |= cyw43_bt_reg_write(data, data->fw_buf.b2h_out, zero);

	return ret ? -EIO : 0;
}

/**
 * Read all four ring indices in a single contiguous backplane read.
 * The four 32-bit registers are laid out consecutively starting at
 * fw_buf.h2b_in, so one mem_read covers all of them.
 *
 * @param data   Driver instance data.
 * @param index  Output: populated with current h2b_in/out, b2h_in/out.
 * @return 0 on success, negative errno on bus error.
 */
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

	index->h2b_in  = values[0];
	index->h2b_out = values[1];
	index->b2h_in  = values[2];
	index->b2h_out = values[3];

	return 0;
}

/**
 * Return the number of bytes available for reading in a power-of-2 ring.
 * Uses unsigned subtraction so wrap-around is handled correctly without
 * a branch.
 */
static uint32_t cyw43_bt_circ_count(uint32_t in, uint32_t out)
{
	return (in - out) & (BTSDIO_FWBUF_SIZE - 1);
}

/**
 * Return the number of bytes free for writing.
 * The +4 reserves space for the BTSDIO packet header so the ring never
 * reports full with only a partial header slot remaining.
 */
static uint32_t cyw43_bt_circ_space(uint32_t in, uint32_t out)
{
	return cyw43_bt_circ_count(out, in + 4);
}

/**
 * Write one HCI packet to the H2B ring buffer.
 *
 * The BTSDIO ring requires 4-byte-aligned writes. If the packet wraps
 * the end of the ring, it is split into two contiguous writes. After
 * writing, h2b_in is updated and the interrupt line is toggled to wake
 * the BT core.
 *
 * @param data    Driver instance data.
 * @param packet  Packet bytes (BTSDIO header + HCI payload).
 * @param len     Total packet length in bytes.
 * @return 0 on success, -ENOBUFS if ring is full, negative errno on error.
 */
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

	LOG_DBG("BT ring before write: h2b_in=%u h2b_out=%u b2h_in=%u b2h_out=%u",
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

/**
 * Read one HCI packet from the B2H ring buffer.
 *
 * The BTSDIO packet format is a 4-byte header [len_lo, len_mid, len_hi,
 * pkt_type] followed by the HCI payload. The header is peeked first to
 * determine total length before the full read. If the packet wraps the
 * ring end, it is reassembled from two reads. After reading, b2h_out is
 * updated and the interrupt line is toggled.
 *
 * @param data      Driver instance data.
 * @param packet    Output buffer for the complete packet (header + payload).
 * @param max_len   Size of the output buffer.
 * @param read_len  Output: total bytes read (header + payload), 0 on no-data.
 * @return 0 on success, -EAGAIN if no data available, -ENOBUFS if packet
 *         exceeds max_len, negative errno on bus error.
 */
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
	if (rounded_len > BTSDIO_FWBUF_SIZE) {
		LOG_DBG("BT invalid B2H packet length %u, dropping %u queued byte(s)",
			payload_len, available);
		ret = cyw43_bt_reg_write(data, data->fw_buf.b2h_out,
					 index.b2h_in);
		if (ret) {
			return ret;
		}
		(void)cyw43_bt_toggle_intr(data);
		return -ENOBUFS;
	}
	if (available < rounded_len) {
		return -EAGAIN;
	}
	if (rounded_len > max_len) {
		LOG_DBG("BT B2H packet too large: payload=%u total=%u rounded=%u max=%u",
			payload_len, total_len, rounded_len, max_len);
		out = (index.b2h_out + rounded_len) & (BTSDIO_FWBUF_SIZE - 1);
		ret = cyw43_bt_reg_write(data, data->fw_buf.b2h_out, out);
		if (ret) {
			return ret;
		}
		(void)cyw43_bt_toggle_intr(data);
		return -ENOBUFS;
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

/**
 * RX thread — polls the B2H ring, allocates net_bufs, and dispatches to
 * the Zephyr BT stack.
 *
 * bt_buf_get_rx() allocates a net_buf and prepends the H:4 type byte
 * automatically, so the HCI payload (buf[4..]) is appended directly after
 * it. Packets with no HCI payload (total_len == 4, header only) and
 * unrecognised packet types are silently dropped.
 *
 * Runs at preemptible priority 5; yields to the BT stack threads during
 * the recv() dispatch.
 */
static void cyw43_bt_rx_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct cyw43_bt_hci_data *data = dev->data;
	uint8_t buf[CYW43_BT_RX_BUF_SIZE] __aligned(4);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t len = 0;
		int ret = cyw43_bt_ring_read(data, buf, sizeof(buf), &len);

		if (ret == -EAGAIN || len <= 4) {
			k_msleep(1);
			continue;
		}

		if (ret) {
			LOG_ERR("BT ring read error: %d", ret);
			k_msleep(1);
			continue;
		}

		uint8_t pkt_type = buf[3];
		struct net_buf *nb = NULL;

		switch (pkt_type) {
		case HCI_PACKET_TYPE_EVENT:
			nb = bt_buf_get_rx(BT_BUF_EVT, K_NO_WAIT);
			break;
		case HCI_PACKET_TYPE_ACL:
			nb = bt_buf_get_rx(BT_BUF_ACL_IN, K_NO_WAIT);
			break;
		default:
			LOG_DBG("BT unknown packet type 0x%02x len %u",
				pkt_type, len);
			continue;
		}

		if (!nb) {
			LOG_ERR("BT no RX buffer for type 0x%02x", pkt_type);
			continue;
		}

		if (net_buf_tailroom(nb) < len - 4) {
			LOG_ERR("BT RX buffer too small for type 0x%02x len %u tailroom %u",
				pkt_type, len - 4, net_buf_tailroom(nb));
			net_buf_unref(nb);
			continue;
		}

		net_buf_add_mem(nb, &buf[4], len - 4);
		data->recv(dev, nb);
	}
}

/**
 * bt_hci_driver_api.open — initialise the CYW43439 BT transport.
 *
 * Sequence:
 *   1. Obtain WHD driver handle (WiFi must already be up).
 *   2. Enable WHD shared BT bus mode.
 *   3. Download BT firmware patch via backplane.
 *   4. Wait for firmware ready signal.
 *   5. Resolve ring buffer addresses and zero indices.
 *   6. Wait for BT core awake signal.
 *   7. Set SW_RDY and toggle interrupt to signal host readiness.
 *   8. Start the RX polling thread.
 *
 * @param dev   Zephyr device handle.
 * @param recv  BT stack receive callback registered by bt_hci_recv().
 * @return 0 on success, negative errno on any initialisation failure.
 */
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

	k_thread_create(&data->rx_thread, cyw43_bt_rx_stack,
			K_THREAD_STACK_SIZEOF(cyw43_bt_rx_stack),
			cyw43_bt_rx_thread, (void *)dev, NULL, NULL,
			CYW43_BT_RX_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&data->rx_thread, "bt_rx");

	LOG_INF("BT HCI transport ready");
	return 0;
}

/**
 * bt_hci_driver_api.send — transmit one HCI packet to the BT core.
 *
 * The Zephyr BT stack prepends the H:4 type byte before calling send().
 * It is stripped here and repackaged into the BTSDIO 4-byte header
 * [len_lo, len_mid, len_hi, pkt_type] before writing to the H2B ring.
 * The BT core is woken before each write via WAKE_BT.
 *
 * @param dev  Zephyr device handle.
 * @param buf  net_buf owned by the BT stack; unreferenced after this call.
 * @return 0 on success, negative errno on error.
 */
static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	struct cyw43_bt_hci_data *data = dev->data;
	uint8_t type = net_buf_pull_u8(buf);  /* H:4 type byte prepended by stack */
	uint32_t payload_len = buf->len;
	uint8_t tx[CYW43_BT_RX_BUF_SIZE] __aligned(4);
	int ret;

	if (4 + payload_len > sizeof(tx)) {
		LOG_ERR("BT send: packet too large (%u)", payload_len);
		net_buf_unref(buf);
		return -EINVAL;
	}

	/* BTSDIO 4-byte header: [len_lo, len_mid, len_hi, packet_type] */
	tx[0] = payload_len & 0xff;
	tx[1] = (payload_len >> 8) & 0xff;
	tx[2] = 0;
	tx[3] = type;
	memcpy(&tx[4], buf->data, payload_len);
	net_buf_unref(buf);

	ret = cyw43_bt_set_bt_awake(data, true);
	if (!ret) {
		ret = cyw43_bt_wait_awake(data);
	}
	if (!ret) {
		ret = cyw43_bt_ring_write(data, tx, 4 + payload_len);
	}

	return ret;
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
