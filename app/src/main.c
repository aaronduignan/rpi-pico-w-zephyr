#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>

int main(void)
{
	int err;

	printk("Pico W - Zephyr testing.\n");
	printk("Initialising Bluetooth...\n");

	err = bt_enable(NULL);

	printk("bt_enable returned: %d\n", err);

	/* Zephyr shell runs in its own thread; returning here is safe. */
	return 0;
}
