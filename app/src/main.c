#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pico_app);

int main(void)
{
	int err;

	LOG_INF("Pico W - Zephyr testing");
	LOG_INF("Initialising Bluetooth...");

	err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable returned: %d", err);
	} else {
		LOG_INF("bt_enable returned: %d", err);
	}

	/* Zephyr shell runs in its own thread; returning here is safe. */
	return 0;
}
