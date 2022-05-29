#ifndef __STM32_GPIO_H
#define __STM32_GPIO_H

#include <stdint.h> // uint32_t

uint8_t check_gpio_valid(uint32_t pin);

struct gpio_out {
    void *regs;
    uint32_t bit;
};
struct gpio_out gpio_out_setup(uint32_t pin, uint32_t val);
void gpio_out_reset(struct gpio_out g, uint32_t val);
void gpio_out_toggle_noirq(struct gpio_out g);
void gpio_out_toggle(struct gpio_out g);
void gpio_out_write(struct gpio_out g, uint32_t val);

struct gpio_in {
    void *regs;
    uint32_t bit;
};
struct gpio_in gpio_in_setup(uint32_t pin, int32_t pull_up);
void gpio_in_reset(struct gpio_in g, int32_t pull_up);
uint8_t gpio_in_read(struct gpio_in g);

struct spi_config {
    void *spi;
    uint32_t spi_cr1;
};
struct spi_config spi_setup(uint32_t bus, uint8_t mode, uint32_t rate);
void spi_prepare(struct spi_config config);
void spi_transfer(struct spi_config config, uint8_t receive_data
                  , uint16_t len, uint8_t *data);
void spi_set_rate(struct spi_config *config, uint32_t rate);

#endif // gpio.h
