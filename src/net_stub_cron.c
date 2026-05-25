/* net_stub_cron.c — Cronopio network seam for Quake (replaces net_drivers.c).
 *
 * Quake always runs a local server that the client reaches over the loopback
 * driver, even in single-player — so we keep net_main.c + net_loop.c and only
 * trim the driver table to loopback alone (the upstream net_drivers.c also
 * registers the Datagram/UDP driver, which we don't build). */

#include "quakedef.h"
#include "net.h"

/* The loopback driver functions live in net_loop.c (kept). Its header is in
 * net/src (not on our include path), so declare what the driver table needs. */
i32        Loop_Init(void);
void       Loop_Listen(qboolean state);
void       Loop_SearchForHosts(qboolean xmit);
qsocket_t* Loop_Connect(char* host);
qsocket_t* Loop_CheckNewConnections(void);
i32        Loop_GetMessage(qsocket_t* sock);
i32        Loop_SendMessage(qsocket_t* sock, sizebuf_t* data);
i32        Loop_SendUnreliableMessage(qsocket_t* sock, sizebuf_t* data);
qboolean   Loop_CanSendMessage(qsocket_t* sock);
qboolean   Loop_CanSendUnreliableMessage(qsocket_t* sock);
void       Loop_Close(qsocket_t* sock);
void       Loop_Shutdown(void);

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
