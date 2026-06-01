#include <zephyr/kernel.h>

int main(void)
{
	printk("Pico W - Zephyr testing.\n");
	/* Zephyr shell runs in its own thread; returning here is safe. */
	return 0;
}
