#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/init.h"
#include "lwip/priv/tcpip_priv.h"
#include "rpmsg_eth.h"

#include "network.h"

static struct netif server_netif;

int network_init(struct rpmsg_device *rdev)
{
    /* initialize lwIP before calling sys_thread_new */
    lwip_init();

    struct netif* netif = &server_netif;

    ip_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 10, 43, 0, 3);
    IP4_ADDR(&netmask, 255, 255, 0, 0);
    IP4_ADDR(&gw, 10, 43, 0, 1);

    /* Add network interface to the netif_list */
    netif_add(netif,
        &ipaddr,
        &netmask,
        &gw,
        rdev,
        rpmsg_eth_init,
        tcpip_input);

    netif_set_default(netif);

    /* specify that the network if is up */
    netif_set_up(netif);

    return 0;
}
