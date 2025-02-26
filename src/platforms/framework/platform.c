/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the platform specific functions for the native implementation. */

#include "general.h"
#include "platform.h"
#include "usb.h"
#include "aux_serial.h"
#include "morse.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/scs.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/timer.h>

static void adc_init(void);
static void setup_vbus_irq(void);

/* This is defined by the linker script */
extern char vector_table;

#define TPWR_SOFT_START_STEPS 64U

/*
 * Starting with hardware version 4 we are storing the hardware version in the
 * flash option user Data1 byte.
 * The hardware version 4 was the transition version that had it's hardware
 * pins strapped to 3 but contains version 4 in the Data1 byte.
 * The hardware 4 is backward compatible with V3 but provides the new jumper
 * connecting STRACE target pin to the UART1 pin.
 * Hardware version 5 does not have the physically strapped version encoding
 * any more and the hardware version has to be read out of the option bytes.
 * This means that older firmware versions that don't do the detection won't
 * work on the newer hardware.
 */
#define BMP_HWVERSION_BYTE FLASH_OPTION_BYTE_2

int platform_hwversion(void)
{
	return 0;
}

void platform_init(void)
{
	SCS_DEMCR |= SCS_DEMCR_VC_MON_EN;

	rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);

	/* Enable peripherals */
	rcc_periph_clock_enable(RCC_USB);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_TIM1);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_CRC);

	/* Setup GPIO ports */
	gpio_clear(USB_PU_PORT, USB_PU_PIN);
	gpio_set_mode(USB_PU_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, USB_PU_PIN);

	gpio_set_mode(JTAG_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, TMS_DIR_PIN | TCK_PIN | TDI_PIN);
	gpio_set_mode(JTAG_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_INPUT_FLOAT, TMS_PIN);

	/* This needs some fixing... */
	/* Toggle required to sort out line drivers... */
	gpio_port_write(GPIOA, 0x8102);
	gpio_port_write(GPIOB, 0x2000);

	gpio_port_write(GPIOA, 0x8182);
	gpio_port_write(GPIOB, 0x2002);

	gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, LED_UART | LED_IDLE_RUN | LED_ERROR);

	/*
	 * Enable nRST output. Original uses a NPN to pull down, so setting the
	 * output HIGH asserts. Mini is directly connected so use open drain output
	 * and set LOW to assert.
	 */
	platform_nrst_set_val(false);
	gpio_set_mode(NRST_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, NRST_PIN);
	
	gpio_set(NRST_SENSE_PORT, NRST_SENSE_PIN);
	gpio_set_mode(NRST_SENSE_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, NRST_SENSE_PIN);

	/*
	 * Enable internal pull-up on PWR_BR so that we don't drive
	 * TPWR locally or inadvertently supply power to the target.
	 */

	gpio_set(PWR_BR_PORT, PWR_BR_PIN);
	gpio_set_mode(PWR_BR_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_OPENDRAIN, PWR_BR_PIN);
	

	/* Configure Timer 1 Channel 3N to allow tpwr to be soft start on hw1+ */
	
	/* The pin mapping is a secondary mapping for the pin. We need to enable that. */
	gpio_primary_remap(AFIO_MAPR_SWJ_CFG_FULL_SWJ, AFIO_MAPR_TIM1_REMAP_PARTIAL_REMAP);
	/*
		* Configure Timer 1 to run the the power control pin PWM and switch the timer on
		* NB: We don't configure the pin mode here but rather we configure it to the alt-mode and back in
		* platform_target_set_power() below due to GD32 errata involving PB2 (AUX serial LED).
		* See §3.7.6 of the GD32F103 Compatability Summary for details.
		*/
	timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
	/* Use PWM mode 1 so that the signal generated is low till it exceeds the set value */
	timer_set_oc3_mode(TIM1, TIM_OCM_PWM1);
	/* Mark the output active-low due to how this drives the target pin */
	timer_set_oc_polarity_low(TIM1, TIM_OC3N);
	timer_enable_oc_output(TIM1, TIM_OC3N);
	timer_set_oc_value(TIM1, TIM_OC3, 0);
	/* Make sure dead-time is switched off as this interferes with the correct waveform generation */
	timer_set_deadtime(TIM1, 0);
	/*
		* Configure for 64 steps which also makes this output a 562.5kHz PWM signal
		* given the lack of prescaling and being a peripheral on APB1 (36MHz)
		*/
	timer_set_period(TIM1, TPWR_SOFT_START_STEPS - 1U);
	timer_enable_break_main_output(TIM1);
	timer_continuous_mode(TIM1);
	timer_update_on_overflow(TIM1);
	timer_enable_counter(TIM1);
	

	adc_init();
	
	/* Set up the NVIC vector table for the firmware */
	SCB_VTOR = (uint32_t)&vector_table; // NOLINT(clang-diagnostic-pointer-to-int-cast)

	platform_timing_init();
	blackmagic_usb_init();

	aux_serial_init();

	setup_vbus_irq();
}

void platform_nrst_set_val(bool assert)
{
	gpio_set(TMS_PORT, TMS_PIN);
	gpio_set_val(NRST_PORT, NRST_PIN, assert);

	if (assert) {
		for (volatile size_t i = 0; i < 10000U; ++i)
			continue;
	}
}

bool platform_nrst_get_val(void)
{
	return gpio_get(NRST_SENSE_PORT, NRST_SENSE_PIN) != 0;
}

bool platform_target_get_power(void)
{
	return !gpio_get(PWR_BR_PORT, PWR_BR_PIN);
}

static inline void platform_wait_pwm_cycle()
{
	while (!timer_get_flag(TIM1, TIM_SR_UIF))
		continue;
	timer_clear_flag(TIM1, TIM_SR_UIF);
}

bool platform_target_set_power(const bool power)
{
	/* If we're on hw1 or newer, and are turning the power on */
	if (power) {
		/* Configure the pin to be driven by the timer */
		gpio_set_mode(PWR_BR_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, PWR_BR_PIN);
		timer_clear_flag(TIM1, TIM_SR_UIF);
		/* Wait for one PWM cycle to have taken place */
		platform_wait_pwm_cycle();
		/* Soft start power on the target */
		for (size_t step = 1U; step < TPWR_SOFT_START_STEPS; ++step) {
			/* Set the new PWM value */
			timer_set_oc_value(TIM1, TIM_OC3, step);
			/* Wait for one PWM cycle to have taken place */
			platform_wait_pwm_cycle();
		}
	}
	/* Set the pin state */
	gpio_set_val(PWR_BR_PORT, PWR_BR_PIN, !power);
	/*
	 * If we're turning power on and running hw1+, now configure the pin back over to GPIO and
	 * reset state timer for the next request
	 */
	if (power) {
		gpio_set_mode(PWR_BR_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_OPENDRAIN, PWR_BR_PIN);
		timer_set_oc_value(TIM1, TIM_OC3, 0U);
	}
	return true;
}

static void adc_init(void)
{
	rcc_periph_clock_enable(RCC_ADC1);

	gpio_set_mode(TPWR_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, TPWR_PIN);

	adc_power_off(ADC1);
	adc_disable_scan_mode(ADC1);
	adc_set_single_conversion_mode(ADC1);
	adc_disable_external_trigger_regular(ADC1);
	adc_set_right_aligned(ADC1);
	adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_239DOT5CYC);
	adc_enable_temperature_sensor();
	adc_power_on(ADC1);

	/* Wait for the ADC to finish starting up */
	for (volatile size_t i = 0; i < 800000U; ++i)
		continue;

	adc_reset_calibration(ADC1);
	adc_calibrate(ADC1);
}

uint32_t platform_target_voltage_sense(void)
{
	/*
	 * Returns the voltage in tenths of a volt (so 33 means 3.3V),
	 * except for hardware version 1.
	 * This function is only needed for implementations that allow the
	 * target to be powered from the debug probe
	 */

	uint8_t channel = 8;
	adc_set_regular_sequence(ADC1, 1, &channel);

	adc_start_conversion_direct(ADC1);

	/* Wait for end of conversion. */
	while (!adc_eoc(ADC1))
		continue;

	uint32_t val = adc_read_regular(ADC1); /* 0-4095 */
	/* Clear EOC bit. The GD32F103 does not automatically reset it on ADC read. */
	ADC_SR(ADC1) &= ~ADC_SR_EOC;
	return (val * 99U) / 8191U;
}

const char *platform_target_voltage(void)
{

	static char ret[] = "0.0V";
	uint32_t val = platform_target_voltage_sense();
	ret[0] = '0' + val / 10U;
	ret[2] = '0' + val % 10U;

	return ret;
}

void platform_request_boot(void)
{
	/* Disconnect USB cable */
	gpio_set_mode(USB_PU_PORT, GPIO_MODE_INPUT, 0, USB_PU_PIN);

	/* Drive boot request pin */
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO12);
	gpio_clear(GPIOB, GPIO12);
}

void platform_target_clk_output_enable(bool enable)
{
	(void)enable;
}

bool platform_spi_init(const spi_bus_e bus)
{
	if (bus == SPI_BUS_EXTERNAL) {
		rcc_periph_clock_enable(RCC_SPI1);
		rcc_periph_reset_pulse(RST_SPI1);
		//platform_target_clk_output_enable(true);
		gpio_set_mode(TCK_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, TCK_PIN);
		gpio_set_mode(TDI_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, TDI_PIN);
		gpio_set(TMS_DIR_PORT, TMS_DIR_PIN);
	} else {
		rcc_periph_clock_enable(RCC_SPI2);
		rcc_periph_reset_pulse(RST_SPI2);
	}

	const uint32_t controller = bus == SPI_BUS_EXTERNAL ? EXT_SPI : AUX_SPI;
	spi_init_master(controller, SPI_CR1_BAUDRATE_FPCLK_DIV_8, SPI_CR1_CPOL_CLK_TO_0_WHEN_IDLE,
		SPI_CR1_CPHA_CLK_TRANSITION_1, SPI_CR1_DFF_8BIT, SPI_CR1_MSBFIRST);
	spi_enable(controller);
	return true;
}

bool platform_spi_deinit(spi_bus_e bus)
{
	spi_disable(bus == SPI_BUS_EXTERNAL ? EXT_SPI : AUX_SPI);

	if (bus == SPI_BUS_EXTERNAL) {
		rcc_periph_clock_disable(RCC_SPI1);
		gpio_set_mode(TCK_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, TCK_PIN);
		gpio_set_mode(TDI_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, TDI_PIN);
		// platform_target_clk_output_enable(false);
	} else
		rcc_periph_clock_disable(RCC_SPI2);
	return true;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
	const uint8_t device = device_select & 0x7fU;
	const bool select = !(device_select & 0x80U);
	uint32_t port = AUX_PORT;
	uint16_t pin;
	switch (device) {
	case SPI_DEVICE_INT_FLASH:
		pin = AUX_FCS;
		break;
	case SPI_DEVICE_EXT_FLASH:
		port = EXT_SPI_CS_PORT;
		pin = EXT_SPI_CS;
		break;
	case SPI_DEVICE_SDCARD:
		pin = AUX_SDCS;
		break;
	case SPI_DEVICE_DISPLAY:
		pin = AUX_DCS;
		break;
	default:
		return false;
	}
	gpio_set_val(port, pin, select);
	return true;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
	return spi_xfer(bus == SPI_BUS_EXTERNAL ? EXT_SPI : AUX_SPI, value);
}

void exti15_10_isr(void)
{
	uint32_t usb_vbus_port;
	uint16_t usb_vbus_pin;

	usb_vbus_port = USB_VBUS_PORT;
	usb_vbus_pin = USB_VBUS_PIN;


	if (gpio_get(usb_vbus_port, usb_vbus_pin))
		/* Drive pull-up high if VBUS connected */
		gpio_set_mode(USB_PU_PORT, GPIO_MODE_OUTPUT_10_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, USB_PU_PIN);
	else
		/* Allow pull-up to float if VBUS disconnected */
		gpio_set_mode(USB_PU_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, USB_PU_PIN);

	exti_reset_request(usb_vbus_pin);
}

static void setup_vbus_irq(void)
{
	uint32_t usb_vbus_port;
	uint16_t usb_vbus_pin;

	usb_vbus_port = USB_VBUS_PORT;
	usb_vbus_pin = USB_VBUS_PIN;
	
	nvic_set_priority(USB_VBUS_IRQ, IRQ_PRI_USB_VBUS);
	nvic_enable_irq(USB_VBUS_IRQ);

	gpio_set(usb_vbus_port, usb_vbus_pin);
	gpio_set(USB_PU_PORT, USB_PU_PIN);

	gpio_set_mode(usb_vbus_port, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, usb_vbus_pin);

	/* Configure EXTI for USB VBUS monitor */
	exti_select_source(usb_vbus_pin, usb_vbus_port);
	exti_set_trigger(usb_vbus_pin, EXTI_TRIGGER_BOTH);
	exti_enable_request(usb_vbus_pin);

	exti15_10_isr();
}
