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

void BlockUnregister(BlockDev *d)
{
    d->gone = true;
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i] != d) continue;
        for (int k = i; k + 1 < g_ndevs; k++) g_devs[k] = g_devs[k + 1];
        g_ndevs--;
        kprintf("[BLOCK] %s removed\n", d->name);
        return;
    }
}
