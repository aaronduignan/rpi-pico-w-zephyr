#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_blinky);

/*
 * The LED is on WL_GPIO0 of the CYW43439, routed through the cyw43-gpio
 * driver. It is exposed as led0 in the board overlay. The WiFi driver must
 * be initialised (via prj.conf) before this GPIO is accessible.
 *
 * The LED is wired between WL_GPIO0 and VCC — active-low (LOW = on).
 * This is declared in the overlay as GPIO_ACTIVE_LOW so set(1) = LED on.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	LOG_INF("LED blinky running");

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("LED configure failed: %d", err);
		return err;
	}

	while (1) {
		int ret = gpio_pin_set_dt(&led, 1);
		LOG_INF("LED ON (err=%d)", ret);
		k_msleep(2000);

		ret = gpio_pin_set_dt(&led, 0);
		LOG_INF("LED OFF (err=%d)", ret);
		k_msleep(2000);
	}

	return 0;
}
