
#include <linux/console.h>
#include <linux/serial_core.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>


static void __iomem * esp32p4_uart_base;

// Offsets of various registers from the base pointer.
#define ESP32_UART_FIFO_OFFSET              0x0000
#define ESP32_UART_AFIFO_STATUS_OFFSET      0x0090
#define ESP32_UART_INT_ST_REG_OFFSET        0x08
#define ESP32_UART_INT_ENA_REG_OFFSET       0x0c
#define ESP32_UART_INT_CLR_REG_OFFSET       0x10

// Bitmasks to read statuses.
#define ESP32_UART_AFIFO_STATUS_TX_FULL       (1 << 0)
#define ESP32_UART_AFIFO_STATUS_TX_EMPTY      (1 << 1)
#define ESP32_UART_INT_ST_REG_RXFIFO_FULL     (1 << 0)
#define ESP32_UART_INT_ST_REG_TXFIFO_EMPTY    (1 << 0)
#define ESP32_UART_INT_ENA_REG_TX_FIFO_EMPTY  (1 << 1)
#define ESP32_UART_INT_ENA_REG_RX_FIFO_FULL   (1 << 0)


static struct uart_port esp32p4_port;
static struct uart_driver esp32p4_uart_driver = {
    .owner       = THIS_MODULE,
    .driver_name = "esp32p4_serial",
    .dev_name    = "ttyS",
    .major       = TTY_MAJOR,
    .minor       = 64,
    .nr          = 1,
};


static void esp32p4_uart_putc(struct uart_port * port, unsigned char c)
{
    while (readl(esp32p4_uart_base + ESP32_UART_AFIFO_STATUS_OFFSET) & ESP32_UART_AFIFO_STATUS_TX_FULL)
    {
        cpu_relax();
    }

    writeb(c, esp32p4_uart_base + ESP32_UART_FIFO_OFFSET);
}


static unsigned int esp32p4_uart_tx_empty(struct uart_port * port)
{
    if (readl(esp32p4_uart_base + ESP32_UART_AFIFO_STATUS_OFFSET) & ESP32_UART_AFIFO_STATUS_TX_EMPTY)
    {
        return TIOCSER_TEMT;
    }
    else
    {
        return 0;
    }
}


static void esp32p4_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
    // Modem control lines (DTR/RTS). You can leave this empty for basic 3-wire UART.
}


static unsigned int esp32p4_uart_get_mctrl(struct uart_port * port)
{
    // Return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS for a fake "always connected" state.
    return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}


static void esp32p4_uart_stop_tx(struct uart_port * port)
{
    unsigned int ena;
    ena = readl(port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
    ena &= ~ESP32_UART_INT_ENA_REG_TX_FIFO_EMPTY;
    writel(ena, port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
}


static void esp32p4_uart_start_tx(struct uart_port * port)
{
    unsigned int ena;
    ena = readl(port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
    ena |= ESP32_UART_INT_ENA_REG_TX_FIFO_EMPTY;
    writel(ena, port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
}


static void esp32p4_uart_stop_rx(struct uart_port * port)
{
    unsigned int ena;
    ena = readl(port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
    ena &= ~ESP32_UART_INT_ENA_REG_RX_FIFO_FULL;
    writel(ena, port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
}


static void esp32p4_uart_shutdown(struct uart_port * port)
{
    // Disable hardware interrupts.
    unsigned int ena;
    ena = readl(port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
    ena &= ~ESP32_UART_INT_ENA_REG_RX_FIFO_FULL;
    ena &= ~ESP32_UART_INT_ENA_REG_TX_FIFO_EMPTY;
    writel(ena, port->membase + ESP32_UART_INT_ENA_REG_OFFSET);

    free_irq(port->irq, port);
}


static void esp32p4_uart_set_termios(struct uart_port * port, struct ktermios * termios, const struct ktermios * old)
{
    // Called to set baud rate, parity, stop bits. 
    // Eventually, calculate the clock divisors here.
}


static const char * esp32p4_uart_type(struct uart_port * port)
{
    return "ESP32-P4 UART";
}
static void esp32p4_uart_release_port(struct uart_port * port)
{ }
static int esp32p4_uart_request_port(struct uart_port * port)
{
    return 0;
}
static void esp32p4_uart_config_port(struct uart_port * port, int flags)
{
    if (flags & UART_CONFIG_TYPE)
    {
        port->type = PORT_UNKNOWN; // Or assign a custom PORT_ number
    }
}


static void esp32p4_transmit_chars(struct uart_port * port)
{
    unsigned char ch;

    uart_port_tx(
        port,
        ch,
        ~(readb(port->membase + ESP32_UART_AFIFO_STATUS_OFFSET) & ESP32_UART_AFIFO_STATUS_TX_FULL),
        writeb(ch, port->membase + ESP32_UART_FIFO_OFFSET)
    );
}


static void esp32p4_uart_early_write(struct console * con, const char * s, unsigned int n)
{
    struct earlycon_device * dev = con->data;
    uart_console_write(&dev->port, s, n, esp32p4_uart_putc);
}


static irqreturn_t esp32p4_uart_interrupt(int irq, void * dev_id)
{
    struct uart_port * port = dev_id;
    unsigned int status;
    unsigned char ch;

    status = readl(port->membase + ESP32_UART_INT_ST_REG_OFFSET);

    if (status & ESP32_UART_INT_ST_REG_RXFIFO_FULL)
    {
        while ((ch = readl(port->membase + ESP32_UART_FIFO_OFFSET)) != 0)
        {
            tty_insert_flip_char(&port->state->port, ch, TTY_NORMAL);
        }
        tty_flip_buffer_push(&port->state->port);
    }

    if (status & ESP32_UART_INT_ST_REG_TXFIFO_EMPTY)
    {
        esp32p4_transmit_chars(port);
    }

    // Clear whatever statuses we had.
    writel(status, port->membase + ESP32_UART_INT_CLR_REG_OFFSET);

    return IRQ_HANDLED;
}


static int esp32p4_uart_startup(struct uart_port *port)
{
    int ret;

    // 1. Ask the kernel for the IRQ now that the system is fully booted
    ret = request_irq(port->irq, esp32p4_uart_interrupt, IRQF_SHARED, "esp32p4_uart", port);
    if (ret) {
        return ret;
    }

    // 2. Wake up the hardware to start listening for RX characters
    unsigned int ena = readl(port->membase + ESP32_UART_INT_ENA_REG_OFFSET);
    ena |= ESP32_UART_INT_ENA_REG_RX_FIFO_FULL;
    writel(ena, port->membase + ESP32_UART_INT_ENA_REG_OFFSET);

    return 0;
}


static int __init esp32p4_early_console_setup(struct earlycon_device *device, const char *options)
{
    if (!device->port.membase)
    {
        return -ENODEV;
    }

    esp32p4_uart_base = device->port.membase;
    device->con->write = esp32p4_uart_early_write;

    return 0;
}


static const struct uart_ops esp32p4_uart_pops = {
    .tx_empty   = esp32p4_uart_tx_empty,
    .set_mctrl  = esp32p4_uart_set_mctrl,
    .get_mctrl  = esp32p4_uart_get_mctrl,
    .stop_tx    = esp32p4_uart_stop_tx,
    .start_tx   = esp32p4_uart_start_tx,
    .stop_rx    = esp32p4_uart_stop_rx,
    .startup    = esp32p4_uart_startup,
    .shutdown   = esp32p4_uart_shutdown,
    .set_termios = esp32p4_uart_set_termios,
    .type       = esp32p4_uart_type,
    .release_port = esp32p4_uart_release_port,
    .request_port = esp32p4_uart_request_port,
    .config_port = esp32p4_uart_config_port,
};


static int esp32p4_uart_probe(struct platform_device * pdev)
{
    struct resource * res_mem;
    int irq;
    int ret;

    res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res_mem)
    {
        return -ENODEV;
    }

    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
    {
        return irq;
    }

    esp32p4_port.dev  = &pdev->dev;
    esp32p4_port.line = 0; // ttyS0
    esp32p4_port.type = PORT_UNKNOWN;
    esp32p4_port.iotype = UPIO_MEM;
    esp32p4_port.mapbase = res_mem->start;
    esp32p4_port.irq = irq;
    esp32p4_port.ops = &esp32p4_uart_pops;
    esp32p4_port.flags = UPF_BOOT_AUTOCONF;

    esp32p4_port.membase = devm_ioremap_resource(&pdev->dev, res_mem);
    if (IS_ERR(esp32p4_port.membase))
    {
        return PTR_ERR(esp32p4_port.membase);
    }

    ret = uart_add_one_port(&esp32p4_uart_driver, &esp32p4_port);
    if (ret)
    {
        return ret;
    }

    platform_set_drvdata(pdev, &esp32p4_port);

    return 0;
}


static const struct of_device_id esp32p4_uart_dt_ids[] = {
    { .compatible = "espressif,esp32p4-uart" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, esp32p4_uart_dt_ids);


static struct platform_driver esp32p4_platform_driver = {
    .probe  = esp32p4_uart_probe,
    .driver = {
        .name           = "esp32p4-uart",
        .of_match_table = esp32p4_uart_dt_ids,
    },
};


static int __init esp32p4_uart_init(void)
{
    int ret;

    // Register the TTY driver core
    ret = uart_register_driver(&esp32p4_uart_driver);
    if (ret)
        return ret;

    // Register the platform driver to start searching the Device Tree
    ret = platform_driver_register(&esp32p4_platform_driver);
    if (ret) {
        uart_unregister_driver(&esp32p4_uart_driver);
        return ret;
    }

    return 0;
}

device_initcall(esp32p4_uart_init);

OF_EARLYCON_DECLARE(esp32p4_uart, "espressif,esp32p4-uart", esp32p4_early_console_setup);
