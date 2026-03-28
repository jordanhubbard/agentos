/*
 * Bare-metal math.h stub for agentOS/seL4 freestanding build.
 */
#ifndef _MATH_H
#define _MATH_H

double fabs(double x);
float  fabsf(float x);
double sqrt(double x);
float  sqrtf(float x);
double ceil(double x);
float  ceilf(float x);
double floor(double x);
float  floorf(float x);
double trunc(double x);
float  truncf(float x);
double rint(double x);
float  rintf(float x);
double fmin(double x, double y);
float  fminf(float x, float y);
double fmax(double x, double y);
float  fmaxf(float x, float y);
double pow(double x, double y);
double log(double x);
double log2(double x);
double exp(double x);
double copysign(double x, double y);
float  copysignf(float x, float y);
double fmod(double x, double y);
float  fmodf(float x, float y);

#ifndef NAN
#define NAN  __builtin_nanf("")
#endif
#ifndef INFINITY
#define INFINITY __builtin_inff()
#endif
#ifndef HUGE_VALF
#define HUGE_VALF __builtin_inff()
#endif
#ifndef HUGE_VAL
#define HUGE_VAL  __builtin_inf()
#endif

/* isnan / isinf / signbit as macros */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#endif /* _MATH_H */
