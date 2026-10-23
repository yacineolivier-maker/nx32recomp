#pragma once
#include "cpu.h"

/* Carte mémoire invitée (espace 32 bits) :
 *   0x00200000-0x03000000 image liée (rtld, main, subsdk*, sdk ; bornes : rt_image_base, rt_image_taille)
 *   0x03000000-0x04000000 région des piles (pile principale + TLS)
 *   0x08000000-0x88000000 tas (le jeu demande 0x78000000 octets)
 *   0x90000000-0xD0000000 région alias */
#define STACK_REGION_BASE 0x03000000u
#define STACK_REGION_SIZE 0x01000000u
#define TLS_BASE        0x03000000u
#define TLS_SIZE        0x00200000u /* 0x200 octets par thread */
#define MAIN_STACK_BASE 0x03800000u
#define MAIN_STACK_SIZE 0x00100000u
#define HEAP_BASE       0x08000000u
#define HEAP_MAX        0x80000000u
#define ALIAS_BASE      0x90000000u
#define ALIAS_SIZE      0x40000000u

#define MAIN_THREAD_HANDLE 0x00010001u

extern const uint32_t rt_image_base, rt_image_taille; /* rt.c */

void svc_init(void);
uint32_t svc_alloc_tls(Cpu *c);
Cpu *svc_current(void);
Cpu *svc_adopt_main(Cpu *c);
void rt_svc(Cpu *c, uint32_t imm);
