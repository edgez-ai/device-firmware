/* Passive register snapshots taken immediately before CPU WFI/WFE. */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_uarte.h>
#include <nrfx.h>

#include "halow_sleep_diag.h"

#if defined(CONFIG_RADAR_HALOW_SLEEP_DIAG)

/* Carrier wiring is fixed by the nRF54L15 camera overlay. */
#define PIN_WAKE NRF_GPIO_PIN_MAP(1, 5)
#define PIN_IRQ NRF_GPIO_PIN_MAP(1, 6)
#define PIN_BUSY NRF_GPIO_PIN_MAP(1, 10)
#define PIN_CS NRF_GPIO_PIN_MAP(1, 7)
#define PIN_CAMERA NRF_GPIO_PIN_MAP(1, 9)
#define SAMPLE_GAP_MS 1000U
#define APPLICATION_QUIET_MS 200U

BUILD_ASSERT(HALOW_SLEEP_DIAG_TEXT_SIZE + 4 <= 256,
	     "Diagnostic resource TLV must fit the configured CoAP block");

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi00));
static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(camera0));

struct snapshot {
	uint32_t ms, silence_ms, pins;
	uint32_t spi_enabled, uart_enabled, spi_pm, uart_pm;
	uint32_t xo_run, xo_stat, pll_run, pll_stat, constlat, dcdc;
	uint32_t debug_request, system_request, irq_pull, busy_pull;
	uint32_t uart_tx_cfg, uart_rx_cfg, uart_interrupts, uart_events;
};

static atomic_t online;
static volatile uint32_t last_activity_ms;
static uint32_t idle_entries, samples, quiet_samples;
static struct snapshot latest, quiet;

void halow_sleep_diag_activity(void)
{
	last_activity_ms = k_uptime_get_32();
}

void halow_sleep_diag_link_changed(bool connected)
{
	unsigned int key = irq_lock();

	/* A new association starts a new diagnostic window. */
	idle_entries = samples = quiet_samples = 0;
	last_activity_ms = k_uptime_get_32();
	atomic_set(&online, connected);
	irq_unlock(key);
}

static uint32_t device_state(const struct device *dev)
{
	enum pm_device_state state;

	if (pm_device_state_get(dev, &state) != 0 || (unsigned int)state > 3U) {
		return 9; /* State unavailable, not evidence that hardware is enabled. */
	}
	return state;
}

bool z_arm_on_enter_cpu_idle(void)
{
	/* Called with interrupts locked. Never block, format, allocate, touch
	 * radio transport, change pins, or veto the architecture's sleep. */
	if (!atomic_get(&online)) return true;
	idle_entries++;
	uint32_t now = k_uptime_get_32();
	uint32_t silence = now - last_activity_ms;

	if (silence < APPLICATION_QUIET_MS ||
	    (samples && now - latest.ms < SAMPLE_GAP_MS)) return true;

	struct snapshot next = {
		.ms = now, .silence_ms = silence,
		.pins = (nrf_gpio_pin_out_read(PIN_WAKE) << 0) |
			(nrf_gpio_pin_read(PIN_BUSY) << 1) |
			(nrf_gpio_pin_read(PIN_IRQ) << 2) |
			(nrf_gpio_pin_out_read(PIN_CS) << 3) |
			(nrf_gpio_pin_out_read(PIN_CAMERA) << 4),
		.spi_enabled = NRF_SPIM00->ENABLE != 0,
		.uart_enabled = NRF_UARTE22->ENABLE != 0,
		.spi_pm = device_state(spi), .uart_pm = device_state(uart),
		.xo_run = NRF_CLOCK->XO.RUN != 0,
		.xo_stat = (NRF_CLOCK->XO.STAT & CLOCK_XO_STAT_STATE_Msk) != 0,
		.pll_run = NRF_CLOCK->PLL.RUN != 0,
		.pll_stat = (NRF_CLOCK->PLL.STAT & CLOCK_PLL_STAT_STATE_Msk) != 0,
		.constlat = NRF_POWER->CONSTLATSTAT != 0,
		.dcdc = NRF_REGULATORS->VREGMAIN.DCDCEN != 0,
		.debug_request = NRF_TAD->DBGPWRUPREQ,
		.system_request = NRF_TAD->SYSPWRUPREQ,
		.irq_pull = (NRF_P1->PIN_CNF[6] & GPIO_PIN_CNF_PULL_Msk) >> GPIO_PIN_CNF_PULL_Pos,
		.busy_pull = (NRF_P1->PIN_CNF[10] & GPIO_PIN_CNF_PULL_Msk) >> GPIO_PIN_CNF_PULL_Pos,
		.uart_tx_cfg = NRF_P1->PIN_CNF[12], .uart_rx_cfg = NRF_P1->PIN_CNF[11],
		.uart_interrupts = NRF_UARTE22->INTENSET,
		.uart_events = (uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_RXSTARTED) << 0 |
			(uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_ENDRX) << 1 |
			(uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_RXTO) << 2 |
			(uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_ENDTX) << 3 |
			(uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_TXSTOPPED) << 4 |
			(uint32_t)nrf_uarte_event_check(NRF_UARTE22, NRF_UARTE_EVENT_ERROR) << 5,
	};
	latest = next;
	samples++;
	if ((next.pins & (BIT(0) | BIT(1) | BIT(4))) == 0) {
		quiet = next;
		quiet_samples++;
	}
	return true;
}

void halow_sleep_diag_report(char *text, size_t size)
{
	unsigned int key = irq_lock();
	uint32_t entries = idle_entries, count = samples, qcount = quiet_samples;
	struct snapshot chosen = qcount ? quiet : latest;
	uint32_t now = k_uptime_get_32();
	bool connected = atomic_get(&online);
	irq_unlock(key);

	if (!count) {
		snprintk(text, size, "v2 preWFI pending online=%u idle=%u", (unsigned int)connected, entries);
		return;
	}
	/* Booleans, bounded PM enums and fixed-width registers keep the worst
	 * case below 250 bytes. No changing multi-block diagnostic representation. */
	int length = snprintk(text, size,
		"v2 preWFI sel=%c t=%u age=%u idle=%u n=%u q=%u sil=%u "
		"w=%u b=%u irq=%u cs=%u cam=%u spi=%u/%u uart=%u/%u "
		"xo=%u/%u pll=%u/%u lat=%u dc=%u dbg=%x/%x ip=%u bp=%u "
		"u=%x/%x ie=%x ev=%x",
		qcount ? 'q' : 'e', chosen.ms, now - chosen.ms, entries, count, qcount,
		chosen.silence_ms, (chosen.pins >> 0) & 1U, (chosen.pins >> 1) & 1U,
		(chosen.pins >> 2) & 1U, (chosen.pins >> 3) & 1U, (chosen.pins >> 4) & 1U,
		chosen.spi_enabled, chosen.spi_pm, chosen.uart_enabled, chosen.uart_pm,
		chosen.xo_run, chosen.xo_stat, chosen.pll_run, chosen.pll_stat,
		chosen.constlat, chosen.dcdc, chosen.debug_request, chosen.system_request,
		chosen.irq_pull, chosen.busy_pull, chosen.uart_tx_cfg, chosen.uart_rx_cfg,
		chosen.uart_interrupts, chosen.uart_events);
	if (length < 0 || (size_t)length >= size) {
		snprintk(text, size, "v2 error=report_overflow");
	}
}

#endif /* CONFIG_RADAR_HALOW_SLEEP_DIAG */
