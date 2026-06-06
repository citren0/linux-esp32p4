#include <linux/console.h>
#include <linux/serial_core.h>
#include <linux/init.h>
#include <linux/io.h>

static void __iomem *esp32p4_uart_base;

// The offsets you just pulled from the TRM
#define ESP32_UART_FIFO_OFFSET         0x0000
#define ESP32_UART_AFIFO_STATUS_OFFSET 0x0090
#define ESP32_UART_TX_AFIFO_FULL       (1 << 0) // Bit 0

static void esp32p4_uart_putc(struct uart_port *port, int c)
{
    /* * 1. Poll the Asynchronous FIFO status register. 
     * As long as Bit 0 is 1 (Full), we spin and wait.
     */
    while (readl(esp32p4_uart_base + ESP32_UART_AFIFO_STATUS_OFFSET) & ESP32_UART_TX_AFIFO_FULL) {
        cpu_relax(); // Tells the CPU we are in a busy-wait loop
    }

    // 2. Write the character directly to the hardware FIFO
    writel(c, esp32p4_uart_base + ESP32_UART_FIFO_OFFSET);
}

static void esp32p4_uart_early_write(struct console *con, const char *s, unsigned int n)
{
    struct earlycon_device *dev = con->data;
    // uart_console_write handles the translation of '\n' to '\r\n' for us
    uart_console_write(&dev->port, s, n, esp32p4_uart_putc);
}

static int __init esp32p4_early_console_setup(struct earlycon_device *device, const char *options)
{
    if (!device->port.membase)
        return -ENODEV;

    esp32p4_uart_base = device->port.membase;
    device->con->write = esp32p4_uart_early_write;

    return 0;
}

EARLYCON_DECLARE(esp32p4_uart, esp32p4_early_console_setup);