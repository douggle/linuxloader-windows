// Cruis'n Blast's link address (the game descriptor's linkIpAddress).
//
// The game takes its own address from the cabinet's "eth0": its socket init
// opens a UDP socket and ioctls SIOCGIFADDR on that name. On a PC whose link
// port is not called eth0 (enp3s0, eth1, a USB port, ...) the lookup fails
// and the game links as 0.0.0.0, so no cabinet ever answers its broadcasts.
//
// The loader answers that lookup with the cabinet's own address out of
// [Network] IP_CAB1..IP_CAB4, picked by the machine ID the game's Network
// Adjustments set (1: IP_CAB1, 2: IP_CAB2, and so on). Without a machine ID
// yet, the NIC's own address is kept when it is one of the set, and IP_CAB1
// answers otherwise. The peers are found by broadcast from that address, on
// the subnet the address is in, so nothing else is configured here.
//
// Both builds run this: Windows maps the game's image at the same addresses
// as Linux, so the hook and its trampoline are the same.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "rawthrills.h"
#include "rtGame.h"
#include "../config/config.h"
#include "../log/log.h"

static uint32_t (*ownIpOrig)(void);
static uint32_t machineIdAddress;
static char logged[64];

// "a.b.c.d" as an in_addr (network order), 0 when it is not one.
static uint32_t parseIPv4(const char *s)
{
    unsigned int a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

// The cabinet's own link address, given what its own lookup found.
static uint32_t linkAddress(uint32_t detected)
{
    const EmulatorConfig *cfg = getConfig();
    const char *cab[4] = {cfg->IpCab1, cfg->IpCab2, cfg->IpCab3, cfg->IpCab4};
    int id = machineIdAddress ? *(int *)(uintptr_t)machineIdAddress : 0;
    const char *own = NULL;

    if (id >= 1 && id <= 4 && cab[id - 1][0])
        own = cab[id - 1];
    else
    {
        // No machine ID (yet): the NIC already carrying one of the set is
        // the cabinet's port, and is left as it is.
        for (int i = 0; i < 4; i++)
            if (cab[i][0] && detected && parseIPv4(cab[i]) == detected)
                return detected;
        if (cab[0][0])
            own = cab[0];
    }
    if (!own)
        return detected;

    if (strcmp(logged, own) != 0)
    {
        log_info("Raw Thrills: link address %s (machine ID %d)", own, id);
        snprintf(logged, sizeof(logged), "%s", own);
    }
    return parseIPv4(own);
}

// The game's own-IP lookup, its answer replaced.
static uint32_t linkOwnIp(void)
{
    uint32_t detected = ownIpOrig ? ownIpOrig() : 0;

    if (!getConfig()->enableNetworkPatches)
        return detected;
    return linkAddress(detected);
}

void rtInstallLink(const RtGame *game)
{
    if (!game->linkIpAddress)
        return;
    if (!getConfig()->enableNetworkPatches || !getConfig()->IpCab1[0])
    {
        log_debug("Raw Thrills: link play left to the game (no [Network] IP_CAB1)");
        return;
    }

    ownIpOrig = rtTrampolineAddress(game->linkIpAddress, game->linkIpPrologue);
    if (!ownIpOrig)
    {
        log_warn("Raw Thrills: link address not hooked, the game keeps its own lookup");
        return;
    }
    machineIdAddress = game->machineIdAddress;
    rtDetourAddress(game->linkIpAddress, linkOwnIp);
}
