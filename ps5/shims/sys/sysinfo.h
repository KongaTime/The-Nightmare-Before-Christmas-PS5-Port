/* PS5 stand-in for <sys/sysinfo.h>.
 *
 * xboxrecomp's win32_compat.c calls sysinfo(2) for GlobalMemoryStatusEx. The
 * PS5 payload SDK has no /proc and no sysinfo(2); the console has a unified
 * 16 GiB pool, so the numbers are reported from fixed constants. Nothing in
 * the guest's boot path makes a decision on them.
 */
#ifndef PS5_SYS_SYSINFO_H
#define PS5_SYS_SYSINFO_H

#include <stdint.h>

struct sysinfo {
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[8];
};

static inline int sysinfo(struct sysinfo *info)
{
    if (!info) return -1;
    const unsigned long unit = 1024ul * 1024ul;      /* mem_unit: MiB */
    const unsigned long total = 16384ul;             /* 16 GiB unified pool */
    const unsigned long free_mb = 8192ul;            /* conservative free */
    info->uptime = 0;
    info->loads[0] = info->loads[1] = info->loads[2] = 0;
    info->totalram = total;
    info->freeram = free_mb;
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 1;
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = (unsigned int)unit;
    return 0;
}

#endif
