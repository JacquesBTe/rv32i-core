#define UART_DATA (*(volatile unsigned int *)0x10001000)

/* DATA writes stall the bus (reg_wr_wait in uart_axil.v) until the
 * transmitter is ready for the next byte, so no STATUS polling here. */
static void uart_putc(char c) {
    UART_DATA = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

volatile unsigned int result;
volatile unsigned int a = 55;
volatile unsigned int b = 3;    /* volatile x volatile: no constant tricks possible */

int main(void) {
    unsigned int sum = 0;
    for (unsigned int i = 1; i <= 10; i++)
        sum += i;
    result = sum;                    /* keep the loop honest too */
    unsigned int prod = a * b;       /* must call __mulsi3 on rv32i */
    result = prod;

    uart_puts("hello from rv32i-core\r\n");
    uart_puts((prod == 165) ? "self-test: PASS\r\n" : "self-test: FAIL\r\n");

    return (prod == 165) ? 0 : 1;
}
