/* net_stub_cron.c — Cronopio network seam for Quake (replaces net_drivers.c).
 *
 * Quake always runs a local server that the client reaches over the loopback
 * driver, even in single-player — so we keep net_main.c + net_loop.c and only
 * trim the driver table to loopback alone (the upstream net_drivers.c also
 * registers the Datagram/UDP driver, which we don't build). */

#include "quakedef.h"
#include "net.h"
#include "net_loop.h"

net_driver_t net_drivers[MAX_NET_DRIVERS] = {
    {
        "Loopback",
        false,
        Loop_Init,
        Loop_Listen,
        Loop_SearchForHosts,
        Loop_Connect,
        Loop_CheckNewConnections,
        Loop_GetMessage,
        Loop_SendMessage,
        Loop_SendUnreliableMessage,
        Loop_CanSendMessage,
        Loop_CanSendUnreliableMessage,
        Loop_Close,
        Loop_Shutdown,
    },
};

i32 net_numdrivers = 1;
