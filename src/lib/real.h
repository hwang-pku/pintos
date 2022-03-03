// Use 17.14 fixed-point number representation by default. Can be changed by modifying PRECISION.
#ifndef __LIB_REAL_H
#define __LIB_REAL_H
#define PRECISION 14
typedef int real;
/* fixed-point real arithmetic routine */
real real_add(real, real);
real real_sub(real, real);
real real_mul(real, real);
real real_div(real, real);
real real_int_add(real, int);
real real_int_sub(real, int);
real real_int_mul(real, int);
real real_int_div(real, int);
real int_to_real(int);
real real_to_int(real); // round to the nearest
#endif