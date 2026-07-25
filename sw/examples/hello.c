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
    return (prod == 165) ? 0 : 1;
}