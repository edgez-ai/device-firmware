/* nRF54L15 side of the IWRL6432 link. See radar_link.h. */
#include <zephyr/kernel.h>
/* PlatformIO may auto-collect src/ even when CMake excludes the transport. */
#if !defined(CONFIG_RADAR_SIMULATE_PRESENCE)
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <hal/nrf_uarte.h>

#include "radar_link.h"
#include "hardware_uart.h"
#include "hardware_memory.h"

LOG_MODULE_REGISTER(radar_link, CONFIG_RADAR_LOG_LEVEL);

#define RADAR_UART_NODE DT_ALIAS(radar_uart)
#define USER_NODE       DT_PATH(zephyr_user)

BUILD_ASSERT(DT_NODE_HAS_STATUS(RADAR_UART_NODE, okay),
	     "the overlay must define the radar-uart alias");

static const struct device *const radar_uart = DEVICE_DT_GET(RADAR_UART_NODE);
/* Active = radar 3.3 V supply on. The radar's NRESET is not connected, so a
 * power cycle is how the nRF restarts it. */
static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(USER_NODE, radar_power_gpios);

/* Optional: the UART RX pin again, as a GPIO (radar-rx-wake-gpios). While the
 * UART is suspended this pin is a level interrupt: the radar sends one 0x00
 * byte ahead of every report, and the low pulse of that byte wakes the CPU.
 * The byte itself is not received; the report follows after the radar's wake
 * lead time (hostReportCfg <wakeLeadMs>). No other wire is needed. On the
 * nRF54L15 only P0 and P1 pins can interrupt and wake the CPU. */
#define HAS_WAKE DT_NODE_HAS_PROP(USER_NODE, radar_rx_wake_gpios)
#if HAS_WAKE
static const struct gpio_dt_spec wake = GPIO_DT_SPEC_GET(USER_NODE, radar_rx_wake_gpios);
static struct gpio_callback wake_callback;
static K_SEM_DEFINE(wake_sem, 0, 1);
static bool wake_irq;
#endif

static struct ring_buf radar_rx_ring;
static K_SEM_DEFINE(rx_sem, 0, 1);
static struct k_spinlock rx_lock;
static uint8_t capture[64];
static size_t capture_len;
static uint32_t received_bytes, transmitted_bytes, dropped_bytes;
static uint32_t error_flags, stopped_count, restart_count;
static int restart_error;
static bool uart_on;

static void uart_callback(const struct device *dev, void *user_data)
{
 ARG_UNUSED(user_data);
 uint8_t data[64];
 while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
  int errors = uart_err_check(dev);
  if (errors > 0) error_flags |= (uint32_t)errors;
  if (!uart_irq_rx_ready(dev)) break;
  int len = uart_fifo_read(dev, data, sizeof(data));
  if (len <= 0) break;
  k_spinlock_key_t key = k_spin_lock(&rx_lock);
  size_t written = ring_buf_put(&radar_rx_ring, data, len);
  size_t save = MIN((size_t)len, sizeof(capture) - capture_len);
  received_bytes += len; dropped_bytes += len - written;
  memcpy(capture + capture_len, data, save); capture_len += save;
  k_spin_unlock(&rx_lock, key);
  k_sem_give(&rx_sem);
 }
}

static int port_write(void *ctx, const uint8_t *data, size_t len)
{
	ARG_UNUSED(ctx);

	if (!IS_ENABLED(CONFIG_RADAR_CONFIGURE_SENSOR)) {
		return -ENOTSUP;
	}
	if (!uart_on) {
		return -EIO;
	}
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(radar_uart, data[i]);
	}
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	transmitted_bytes += (uint32_t)len;
	k_spin_unlock(&rx_lock, key);
	return 0;
}

static int port_read(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	ARG_UNUSED(ctx);

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&rx_lock);
		uint32_t got = ring_buf_get(&radar_rx_ring, data, (uint32_t)len);
		k_spin_unlock(&rx_lock, key);

		if (got > 0U) {
			return (int)got;
		}
		if (k_sem_take(&rx_sem, K_MSEC(timeout_ms)) != 0) {
			/* One last look: bytes may have arrived with the timeout. */
			key = k_spin_lock(&rx_lock);
			got = ring_buf_get(&radar_rx_ring, data, (uint32_t)len);
			k_spin_unlock(&rx_lock, key);
			return (int)got;
		}
	}
}

static void port_flush(void *ctx)
{
	ARG_UNUSED(ctx);

	/* Discard queued bytes without interrupting interrupt-driven reception. */
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	ring_buf_reset(&radar_rx_ring);
	k_sem_reset(&rx_sem);
	k_spin_unlock(&rx_lock, key);
}

void radar_link_log_diagnostics(void)
{
	uint8_t raw[sizeof(capture)];
	size_t len;
	uint32_t rx, tx, dropped, errors, stopped, restarts;
	int error;
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	rx = received_bytes;
	tx = transmitted_bytes;
	dropped = dropped_bytes;
	errors = error_flags;
	stopped = stopped_count;
	restarts = restart_count;
	error = restart_error;
	len = capture_len;
	memcpy(raw, capture, len);
	k_spin_unlock(&rx_lock, key);
	LOG_INF("UART diag: tx=%u rx=%u dropped=%u errors=0x%x stopped=%u restarts=%u rx_error=%d RX_pin=%d",
		tx, rx, dropped, errors, stopped, restarts, error,
		gpio_pin_get_raw(power.port, 12));
	if (len != 0U) {
		LOG_HEXDUMP_INF(raw, len, "first received UART bytes");
	}
}

static const struct iwrl_port port = {
	.write = port_write,
	.read = port_read,
	.flush_rx = port_flush,
	.ctx = NULL,
};

const struct iwrl_port *radar_link_port(void)
{
	return &port;
}

/* TI erratum DIG #3 test: match the radar's real baud rate. The 16 MHz
 * Zephyr path only accepts preset rates, so the driver's custom-divisor
 * formula is applied after every resume, before RX/TX starts. */
static void apply_baud(void)
{
	if (IS_ENABLED(CONFIG_RADAR_TI_BAUD_TEST)) {
		BUILD_ASSERT(DT_SAME_NODE(RADAR_UART_NODE, DT_NODELABEL(uart22)),
			     "TI baud test is specific to UART22");
		BUILD_ASSERT(UARTE22_CORE_FREQUENCY == 16,
			     "Recalculate the baud divisor if the UART clock changes");
		BUILD_ASSERT(!IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME),
			     "Runtime PM could replace the custom baud setting");
		NRF_UARTE_Type *regs = (NRF_UARTE_Type *)DT_REG_ADDR(RADAR_UART_NODE);
		const uint32_t divisor = DIV_ROUND_CLOSEST(16000000U, 113636U);
		const uint32_t baud_reg = (BIT(20) / divisor) << 12;

		nrf_uarte_disable(regs);
		nrf_uarte_baudrate_set(regs, (nrf_uarte_baudrate_t)baud_reg);
		nrf_uarte_enable(regs);
	}
}

/* Resume the UART and start interrupt-driven reception. */
static int rx_start(void)
{
	int rc;

	if (uart_on) {
		return 0;
	}
	rc = pm_device_action_run(radar_uart, PM_DEVICE_ACTION_RESUME);
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_ERR("UART resume failed: %d", rc);
		return rc;
	}
	apply_baud();
	uart_irq_rx_enable(radar_uart);
	uart_irq_err_enable(radar_uart);
	uart_on = true;
	return 0;
}

/* Stop reception and suspend the UART, which also releases its pins. Bytes
 * still held by the driver are delivered to the ring buffer first. */
static int rx_stop(void)
{
	int rc;

	if (!uart_on) {
		return 0;
	}
	uart_on = false;
	uart_irq_rx_disable(radar_uart);
	uart_irq_err_disable(radar_uart);
	rc = pm_device_action_run(radar_uart, PM_DEVICE_ACTION_SUSPEND);
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_ERR("UART suspend failed: %d", rc);
		return rc;
	}
	return 0;
}

int radar_link_rx_enable(bool enable)
{
	return enable ? rx_start() : rx_stop();
}

bool radar_link_has_wake(void)
{
#if HAS_WAKE
	return wake_irq;
#else
	return false;
#endif
}

#if HAS_WAKE
static void wake_isr(const struct device *dev, struct gpio_callback *callback, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	/* Level triggered: keep it from firing again while the line is low. */
	(void)gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_DISABLE);
	k_sem_give(&wake_sem);
}

/* Check that the RX pin can interrupt. The pin stays with the UART here; it
 * is only taken over as a GPIO inside radar_link_wait_wake. */
static int wake_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&wake)) {
		LOG_ERR("radar RX wake GPIO not ready");
		return -ENODEV;
	}
	gpio_init_callback(&wake_callback, wake_isr, BIT(wake.pin));
	rc = gpio_add_callback(wake.port, &wake_callback);
	if (rc == 0) {
		rc = gpio_pin_configure_dt(&wake, GPIO_INPUT);
	}
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_LEVEL_ACTIVE);
		(void)gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_DISABLE);
	}
	if (rc == 0) {
		wake_irq = true;
		LOG_INF("UART RX %s pin %u wakes the CPU while the UART is off",
			wake.port->name, wake.pin);
	} else {
		wake_irq = false;
		LOG_WRN("UART RX %s pin %u cannot interrupt (%d): the UART stays on",
			wake.port->name, wake.pin, rc);
	}
	return 0;
}
#endif

int radar_link_wait_wake(uint32_t timeout_ms)
{
#if HAS_WAKE
	int rc;

	if (!wake_irq) {
		return 1;
	}
	if (uart_on) {
		/* The UART owns the pin and receives by itself. */
		return -EBUSY;
	}
	/* Suspending the UART disconnected the pin: make it an input with the
	 * pull-up again, then arm it. A line that is already low fires at once. */
	rc = gpio_pin_configure_dt(&wake, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}
	k_sem_reset(&wake_sem);
	rc = gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_LEVEL_ACTIVE);
	if (rc != 0) {
		return rc;
	}
	rc = k_sem_take(&wake_sem, K_MSEC(timeout_ms));
	(void)gpio_pin_interrupt_configure_dt(&wake, GPIO_INT_DISABLE);
	return (rc == 0) ? 1 : 0;
#else
	ARG_UNUSED(timeout_ms);
	return 1;
#endif
}

/* Restart the radar. The UART is suspended while the supply is off, so the
 * unpowered radar is not fed through its I/O pins. */
int radar_link_power_cycle(void)
{
	int rc = rx_stop();

	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_set_dt(&power, 0);
	if (rc != 0) {
		LOG_ERR("radar supply off failed: %d", rc);
		return rc;
	}
	k_msleep(CONFIG_RADAR_POWER_OFF_MS);
	rc = gpio_pin_set_dt(&power, 1);
	if (rc != 0) {
		LOG_ERR("radar supply on failed: %d", rc);
		return rc;
	}
	LOG_INF("radar supply on; waiting %d ms before UART",
		CONFIG_RADAR_POWER_ON_SETTLE_MS);
	k_msleep(CONFIG_RADAR_POWER_ON_SETTLE_MS);
	port_flush(NULL);
	rc = rx_start();
	if (rc == 0) {
		LOG_INF("UART IRQ RX ready: %u baud, TX=P1.11 RX=P1.12, interrupt-driven RX",
			IS_ENABLED(CONFIG_RADAR_TI_BAUD_TEST) ? 113475U : 115200U);
	}
	return rc;
}

/* The CLI-configured profile starts the sensor in its worker thread. */
int radar_link_init(void)
{
	int rc;
	uint8_t *rx = hardware_memory_uart(HARDWARE_RADAR);
	if (!rx) return -ENOMEM;
	ring_buf_init(&radar_rx_ring, CONFIG_RADAR_UART_RX_BUFFER_SIZE, rx);

	if (!gpio_is_ready_dt(&power)) {
		LOG_ERR("radar UART or GPIO not ready");
		return -ENODEV;
	}
	rc = gpio_pin_configure_dt(&power, IS_ENABLED(CONFIG_RADAR_CONFIGURE_SENSOR)
				   ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE);
	if (rc != 0) {
		return rc;
	}
	rc = hardware_uart_init();
	if (rc) return rc;
	rc = uart_irq_callback_user_data_set(radar_uart, uart_callback, NULL);
	if (rc != 0) {
		LOG_ERR("UART IRQ setup failed: %d", rc);
		return rc;
	}
	rc = pm_device_action_run(radar_uart, PM_DEVICE_ACTION_SUSPEND);
	if ((rc != 0) && (rc != -EALREADY)) {
		LOG_ERR("UART suspend failed: %d", rc);
		return rc;
	}
#if HAS_WAKE
	rc = wake_init();
	if (rc != 0) {
		return rc;
	}
#endif
	if (IS_ENABLED(CONFIG_RADAR_CONFIGURE_SENSOR)) {
		return 0;
	}
	LOG_INF("radar supply on; waiting %d ms before UART", CONFIG_RADAR_POWER_ON_SETTLE_MS);
	k_msleep(CONFIG_RADAR_POWER_ON_SETTLE_MS);
	return rx_start();
}

#endif /* !CONFIG_RADAR_SIMULATE_PRESENCE */
