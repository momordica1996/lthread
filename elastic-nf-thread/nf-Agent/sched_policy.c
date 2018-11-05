#include <stdio.h>
static unsigned cyc_hi = 0;
static unsigned cyc_lo = 0;

/* Set *hi and *lo to the high and low order bits of the cycle counter.
 *    Implementation requires assembly code to use the rdtsc instruction. */
void access_counter(unsigned *hi, unsigned *lo)
{
    asm("rdtsc; movl %%edx, %0; movl %%eax, %1"
    : "=r" (*hi), "=r" (*lo)
    : /* No input */
    : "%edx", "%eax");
    return;
}

/* Record the current value of the cycle counter. */
void start_counter(void)
{
    access_counter(&cyc_hi, &cyc_lo);
    return;
}

/* Return the number of cycles since the last call to start_counter. */
double get_counter(void)
{
    unsigned int    ncyc_hi, ncyc_lo;
    unsigned int    hi, lo, borrow;
    double  result;

    /* Get cycle counter */
    access_counter(&ncyc_hi, &ncyc_lo);

    /* Do double precision subtraction */
    lo = ncyc_lo - cyc_lo;
    borrow = lo > ncyc_lo;
    hi = ncyc_hi - cyc_hi - borrow;

    result = (double)hi * (1 << 30) * 4 + lo;
    if (result < 0) {
        printf("Error: counter returns neg value: %.0f\n", result);
    }
    return result;
}
int main(void){

    start_counter();
    int i;
    for(i = 0;i<20;i++)
        i*=2;
    double cnt = get_counter();
    printf("tmp. clk counter = %lf.\n",cnt);
}