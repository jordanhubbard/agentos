#pragma once
#include <assert.h>
#include <stdio.h>
#include <sel4/sel4.h>
#include <sddf/util/util.h>
#define BIT_LOW(n) (1ul << (n))
#define BIT_HIGH(n) (1ul << ((n) - 32))
void vmm_notify(seL4_CPtr channel);
