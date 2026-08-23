#define TIMER_MTIME_LO    (*(volatile unsigned int *)0x10002000)
#define TIMER_MTIME_HI    (*(volatile unsigned int *)0x10002004)
#define TIMER_MTIMECMP_LO (*(volatile unsigned int *)0x10002008)
#define TIMER_MTIMECMP_HI (*(volatile unsigned int *)0x1000200C)

#define GPIO_LED (*(volatile unsigned int *)0x10000000)

#define MIE_MTIE    (1u << 7)
#define MSTATUS_MIE (1u << 3)

static const unsigned int INTERVAL = 4000;   /* cycles between interrupts */

static volatile unsigned int handler_count = 0;
static volatile unsigned int spin_count    = 0;

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
    handler_count++;
    GPIO_LED = handler_count;
    rearm();
}

int main(void) {
    asm volatile ("csrw mtvec, %0" :: "r"(&trap_handler));

    rearm();

    asm volatile ("csrw mie, %0" :: "r"((unsigned int)MIE_MTIE));
    asm volatile ("csrs mstatus, %0" :: "r"((unsigned int)MSTATUS_MIE));

    /* Spin until the handler has run a few times, proving the interrupt
     * fires periodically and this loop keeps making progress in between. */
    while (handler_count < 5) {
        spin_count++;
    }

    /* handler_count may have ticked past 5 between the loop's last check
     * and the interrupt that pushed it over -- that's still success. */
    return (spin_count > 0 && handler_count >= 5) ? 0 : 1;
}
