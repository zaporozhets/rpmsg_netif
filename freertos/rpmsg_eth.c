#include "rpmsg_eth.h"

#include <stdarg.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <openamp/open_amp.h>
#include <openamp/version.h>
#include <metal/alloc.h>
#include <metal/log.h>
#include <metal/version.h>

#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/sys.h"

// The OpenAMP message size is limited by the buffer size defined in the rpmsg kernel module.
// For the Linux 4.19 kernel, this is currently defined as 512 bytes with 16 bytes
// for the message header and 496 bytes of payload.
#define RPMSG_SIZE 496

//MAC ADDRESS is 6 (DEST MAC ADDRESS) +6 (SORUCE MAC ADDRESS) +2 (EtherTYPE)
//Next release will remove the MAC ADDRESS info, it is not needed
#define MTU_RPMSG (RPMSG_SIZE - 6 - 6 - 2)


#define IFNAME0 'e'
#define IFNAME1 'n'

struct rpmsg_eth_priv {
    struct rpmsg_endpoint lept;
    struct netif* netif;
    struct pbuf* rx_pbuf;
};

u32 xInsideISR = 0; // Used by lwip stack


static int rpmsg_endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv);
static void rpmsg_service_unbind(struct rpmsg_endpoint *ept);
static void rpmsg_func(void *unused_arg);
static err_t low_level_output(struct netif* netif, struct pbuf* p);

err_t rpmsg_eth_init(struct netif* netif)
{
    struct rpmsg_device *rpdev = netif->state;

    struct rpmsg_eth_priv* mailboxif;

    LWIP_ASSERT("netif != NULL", (netif != NULL));

    mailboxif = mem_malloc(sizeof(struct rpmsg_eth_priv));
    if (mailboxif == NULL) {
        LWIP_DEBUGF(NETIF_DEBUG, ("ipc_context: out of memory\n"));
        return ERR_MEM;
    }

#if LWIP_NETIF_HOSTNAME
    /* Initialize interface hostname */
    netif->hostname = "nuc472";
#endif /* LWIP_NETIF_HOSTNAME */

    netif->state = mailboxif;

    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    /* We directly use etharp_output() here to save a function call.
     * You can instead declare your own function an call etharp_output()
     * from it if you have to do some checks before sending (e.g. if link
     * is available...) */
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    netif->mtu = MTU_RPMSG;
    netif->hwaddr_len = 6;

    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP | NETIF_FLAG_LINK_UP;

    uint8_t hwaddr[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    netif->hwaddr[0] = hwaddr[0];
    netif->hwaddr[1] = hwaddr[1];
    netif->hwaddr[2] = hwaddr[2];
    netif->hwaddr[3] = hwaddr[3];
    netif->hwaddr[4] = hwaddr[4];
    netif->hwaddr[5] = hwaddr[5] ^ 1;
    netif->hwaddr_len = 6;

    int status = rpmsg_create_ept(&mailboxif->lept, rpdev, "rpmsg-eth",
				       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
				       rpmsg_endpoint_cb,
				       rpmsg_service_unbind);
    if (status) {
        return ERR_IF;
    }

    mailboxif->netif = netif;
    mailboxif->lept.priv = mailboxif;

    return ERR_OK;
}

static int rpmsg_endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
			     uint32_t src, void *priv)
{
    (void)ept;
	(void)src;
    struct rpmsg_eth_priv* rpmsg_eth = (struct rpmsg_eth_priv*)priv;
    struct netif* netif = rpmsg_eth->netif;
    struct eth_hdr* ethhdr;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == NULL) {
        return ERR_MEM;
    }

    pbuf_take(p, data, len);

    ethhdr = (struct eth_hdr*)p->payload;

    switch (htons(ethhdr->type)) {
    /* IP or ARP packet? */
    case ETHTYPE_IP:
    case ETHTYPE_ARP:
#if PPPOE_SUPPORT
    /* PPPoE packet? */
    case ETHTYPE_PPPOEDISC:
    case ETHTYPE_PPPOE:
#endif /* PPPOE_SUPPORT */
        /* full packet send to tcpip_thread to process */
        if (netif->input(p, netif) != ERR_OK) {
            LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));
            pbuf_free(p);
            p = NULL;
        }
        break;
    default:
        pbuf_free(p);
        break;
    }


	return RPMSG_SUCCESS;
}

static void rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
	(void)ept;
}

static err_t low_level_output(struct netif* netif, struct pbuf* p)
{
    struct rpmsg_eth_priv* rpmsg_eth = (struct rpmsg_eth_priv*)netif->state;
    struct pbuf *q;

#if ETH_PAD_SIZE
    pbuf_header(p, -ETH_PAD_SIZE); /* drop the padding word */
#endif

    for (q = p; q != NULL; q = q->next)
    {
        // /* Send data back to master */
        if (rpmsg_send(&rpmsg_eth->lept, q->payload, q->len) < 0) {
            //ML_ERR("rpmsg_send failed\r\n");
            return ERR_BUF;
        }
    }

#if ETH_PAD_SIZE
    pbuf_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

    LINK_STATS_INC(link.xmit);

    return ERR_OK;
}