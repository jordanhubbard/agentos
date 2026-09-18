#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sel4/sel4.h>
#define LOG_VMM_ERR(...) ((void)0)
#define LOG_VMM(...) ((void)0)
#define BIT_LOW(n) (1ul << (n))
#define BIT_HIGH(n) (1ul << ((n)-32))
