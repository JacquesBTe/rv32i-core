#define GPIO_LED (*(volatile unsigned int *)0x10000000)
#define GPIO_SW  (*(volatile unsigned int *)0x10000004)

int main(void) {
    while (1) {
        GPIO_LED = GPIO_SW;
    }
    return 0;
}
