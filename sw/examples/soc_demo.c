#define GPIO_LED (*(volatile unsigned int *)0x10000000)
#define GPIO_SW  (*(volatile unsigned int *)0x10000004)

#define UART_DATA (*(volatile unsigned int *)0x10001000)

#define TIMER_MTIME_LO    (*(volatile unsigned int *)0x10002000)
#define TIMER_MTIME_HI    (*(volatile unsigned int *)0x10002004)
#define TIMER_MTIMECMP_LO (*(volatile unsigned int *)0x10002008)
#define TIMER_MTIMECMP_HI (*(volatile unsigned int *)0x1000200C)

#define MIE_MTIE    (1u << 7)
#define MSTATUS_MIE (1u << 3)

/* One second at 75 MHz. Override with -DINTERVAL=... for a short run in
 * simulation; the real board build uses this default. */
#ifndef INTERVAL
#define INTERVAL 75000000u
#endif

static void uart_putc(char c) {
    UART_DATA = (unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

static void rearm(void) {
    unsigned int lo = TIMER_MTIME_LO;
    unsigned int hi = TIMER_MTIME_HI;
    unsigned long long now  = ((unsigned long long)hi << 32) | lo;
    unsigned long long next = now + INTERVAL;
    TIMER_MTIMECMP_HI = (unsigned int)(next >> 32);
    TIMER_MTIMECMP_LO = (unsigned int)next;
}

__attribute__((interrupt("machine")))
void trap_handler(void) {
    uart_puts("tick\r\n");
    rearm();
}

int main(void) {
    uart_puts("rv32i-core SoC: full system demo\r\n");
    uart_puts("switches drive the LEDs; a UART tick prints periodically\r\n");

    asm volatile ("csrw mtvec, %0" :: "r"(&trap_handler));
    rearm();
    asm volatile ("csrw mie, %0" :: "r"((unsigned int)MIE_MTIE));
    asm volatile ("csrs mstatus, %0" :: "r"((unsigned int)MSTATUS_MIE));

    while (1) {
        GPIO_LED = GPIO_SW;
    }
}
