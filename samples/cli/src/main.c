#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pico_app);

int main(void)
{
	LOG_INF("Pico W ready");
	return 0;
}
