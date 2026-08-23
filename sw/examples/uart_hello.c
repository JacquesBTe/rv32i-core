#define UART_DATA (*(volatile unsigned int *)0x10001000)

/* DATA writes stall the bus (reg_wr_wait in uart_axil.v) until the
 * transmitter is ready for the next byte, so there is no need to poll
 * STATUS.tx_busy here -- the bus does it for us. */
static void uart_putc(char c) {
    UART_DATA = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

int main(void) {
    while (1) {
        uart_puts("hello from rv32i-core\r\n");
        /* Crude busy-wait between lines -- no timer until phase 5 step 9. */
        for (volatile int i = 0; i < 2000000; i++) {}
    }
    return 0;
}
