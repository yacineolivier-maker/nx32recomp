#pragma once
#include "cpu.h"

typedef struct { uint32_t addr; GuestFn fn; } RcEntry;
extern const RcEntry rc_table[];
extern const uint32_t rc_table_size;

GuestFn rt_lookup(uint32_t addr);
void rt_hle_register(uint32_t addr, GuestFn fn);
void rt_fatal(Cpu *c, const char *fmt, ...);
void *rt_commit(uint32_t addr, uint32_t size);
