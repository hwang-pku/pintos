#include<real.h>
#include <stdint.h>
static int offset = (1 << PRECISION);
real real_add(real a, real b)
{
    return a + b;
}
real real_sub(real a, real b)
{
    return a - b;
}
real real_mul(real a, real b)
{
    return ((int64_t) a) * b / offset;
}
real real_div(real a, real b)
{
    return ((int64_t) a) * offset / b;
}
real real_int_add(real a, int n)
{
    return a + n * offset;
}
real real_int_sub(real a, int n)
{
    return a - n * offset;
}
real real_int_mul(real a, int n)
{
    return a * n;
}
real real_int_div(real a, int n)
{
    return a / n;
}
real int_to_real(int n)
{
    return n * offset;
}
real real_to_int(real a) // round to the nearest
{
    return a >= 0 ? (a + offset / 2) / offset : (a - offset / 2) / offset;
}

real real_to_int_down(real); // round down given a positive number
{
    return a / offset;
}