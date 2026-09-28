/*
 * block.c — the list of block devices (see block.h)
 */

#include "block.h"
#include "../ke/printf.h"

static BlockDev *g_devs[BLOCK_MAX_DEVICES];
static int g_ndevs;

void BlockRegister(BlockDev *d)
{
    if (g_ndevs >= BLOCK_MAX_DEVICES) return;
    g_devs[g_ndevs++] = d;
    kprintf("[BLOCK] %s: %llu MiB  %s\n", d->name, (unsigned long long)(d->sectors / 2048), d->model);
}

int BlockCount(void) { return g_ndevs; }
BlockDev *BlockGet(int i) { return i >= 0 && i < g_ndevs ? g_devs[i] : NULL; }
