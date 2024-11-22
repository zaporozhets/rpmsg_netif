/* 
    2024, Taras Zaporozhets
    Based on: 
     rpmsg-net from James Tavares
     rpmsg-neo from Tim Michals

*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>


// The OpenAMP message size is limited by the buffer size defined in the rpmsg kernel module.
// For the Linux 4.19 kernel, this is currently defined as 512 bytes with 16 bytes
// for the message header and 496 bytes of payload.
#define RPMSG_SIZE 496

//MAC ADDRESS is 6 (DEST MAC ADDRESS) +6 (SORUCE MAC ADDRESS) +2 (EtherTYPE)
//Next release will remove the MAC ADDRESS info, it is not needed
#define MTU_RPMSG (RPMSG_SIZE - 6 - 6 - 2)



struct rpmsg_eth_private {
    struct rpmsg_device *rpdev;
    struct net_device *netdev;
    struct net_device_stats stats;

    /**
     * A work queue to process net device transmit events (Net->RPMsg) packets in a process context,
     * rather than the ISR context provided by the net xmit callback.
     */
    struct work_struct immediate;

    /**
     * A work queue to provide for net device transmit retries in the event that the RPMsg ring
     * buffer is full at the time we attempt to transmit.
     */
    struct delayed_work delayed;

    /** The current skb that is being transmitted from net device to RPMsg */
    struct sk_buff *skb;

    /** A boolean indicating that we are retrying a failed skb transmission */
    bool is_delayed;

    /** A lock for the net device transmitter. Used to avoid race condition on shutdown */
    spinlock_t shutdown_lock;

    /** A flag indicating whether the interface is shutdown, or not */
    bool is_shutdown;
};

static void net_xmit_delayed_work_handler(struct work_struct *work);
static void net_xmit_work_handler(struct work_struct *work);
static netdev_tx_t rpmsg_eth_xmit(struct sk_buff *skb, struct net_device *ndev);


static netdev_tx_t rpmsg_eth_xmit(struct sk_buff *skb,
                            struct net_device *dev)
{
    struct rpmsg_eth_private *priv = netdev_priv(dev);
    unsigned long flags;

    // stop the net device transmitter. will be re-enabled by the work queue.
    netif_stop_queue(priv->netdev);

    // schedule work, unless we are shutdown
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (!priv->is_shutdown) {
        priv->skb = skb;
        schedule_work(&priv->immediate);
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);
    } else {
        // we're shut down. drop packet. leave queue stopped.
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);

        dev_consume_skb_any(skb);
        dev_info(&priv->rpdev->dev, "net_xmit: dropping packet due to shutdown request (race)\n");
    }

    priv->stats.tx_packets++;
    priv->stats.tx_bytes += skb->len;

    return NETDEV_TX_OK;
}

static void net_xmit_delayed_work_handler(struct work_struct *work)
{
    struct rpmsg_eth_private *priv = container_of(work, struct rpmsg_eth_private, delayed.work);
    unsigned long flags;

    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (priv->is_delayed && !priv->is_shutdown) {
        schedule_work(&priv->immediate);
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);
    } else {
        spin_unlock_irqrestore(&priv->shutdown_lock, flags);

        dev_info(&priv->rpdev->dev, "delayed work handler skipping kick of immediate due to shutdown request\n");
    }
}

static void net_xmit_work_handler(struct work_struct *work)
{
    struct rpmsg_eth_private *priv = container_of(work, struct rpmsg_eth_private, immediate);
    unsigned long flags;
    int err;

    if (priv->skb == NULL) {
        dev_err(&priv->rpdev->dev, "net_xmit_work_handler called with NULL skb\n");
        goto cleanup;
    }

    err = rpmsg_trysend(priv->rpdev->ept, priv->skb->data, priv->skb->len);
    if (err) {
        if (priv->is_delayed) {
            // this is already our second attempt
            dev_err(&priv->rpdev->dev, "RPMsg send retry failed with error %d; dropping packet\n", err);

            // normal cleanup
            goto cleanup;
        } else {
            // first attempt failed; attempt retry if not shutdown
            spin_lock_irqsave(&priv->shutdown_lock, flags);
            if (!priv->is_shutdown) {
                priv->is_delayed = true;
                // Our goal is to sleep long enough to free at least one (1) packet in the RPMsg
                // ring buffer. When HZ is 100 (lowest setting), our minimum resolution is 10ms (1
                // jiffy). On Kestrel-M4, flood ping clocked in at ~600 1400-bytes packets per
                // second on an unloaded system, and ~200 packets/sec on a loaded system. So, 10ms
                // should give us at least one packet.
                schedule_delayed_work(&priv->delayed, (unsigned long)(0.5 + (0.010 * HZ)));
                spin_unlock_irqrestore(&priv->shutdown_lock, flags);

                dev_err(&priv->rpdev->dev, "RPMsg send failed with error %d; will retry\n", err);
            } else {
                spin_unlock_irqrestore(&priv->shutdown_lock, flags);

                dev_info(&priv->rpdev->dev, "skipping RPMsg send retry due to shutdown request\n");
            }

            // leave priv->skb populated, either for retry, or if under shutdown, to be freed by
            // rpmsg_remove()
            return;
        }
    }

cleanup:
    // return the skb to the network stack
    if (priv->skb != NULL) {
        dev_consume_skb_any(priv->skb);
        priv->skb = NULL;
    }

    priv->is_delayed = false;

    // if not shutdown, re-activate the network stack xmit queue
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    if (!priv->is_shutdown) {
        netif_wake_queue(priv->netdev);
    }
    spin_unlock_irqrestore(&priv->shutdown_lock, flags);
}

static int rpmsg_eth_open(struct net_device *ndev)
{
    netif_start_queue(ndev);
    return 0;
}

int rpmsg_eth_stop (struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}


struct net_device_stats *rpmsg_eth_stats(struct net_device *dev)
{
    struct rpmsg_eth_private *priv = netdev_priv(dev);
    return &priv->stats;
}

static int rpmsg_eth_rx_cb(struct rpmsg_device *rpdev, void *data, int len, void *drv_priv, u32 src)
{
    struct rpmsg_eth_private *priv = dev_get_drvdata(&rpdev->dev);
    struct sk_buff *skb;

    skb = dev_alloc_skb(len + 2);

    skb_reserve(skb, 2); /* align IP on 16B boundary */
    memcpy(skb_put(skb, len), data, len);

    skb->dev = priv->netdev;
    skb->protocol = eth_type_trans(skb, priv->netdev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += len;
    netif_rx(skb);

    return 0;
}

static const struct net_device_ops netdev_ops = {
    .ndo_open           = rpmsg_eth_open,
    .ndo_stop           = rpmsg_eth_stop,
    .ndo_start_xmit     = rpmsg_eth_xmit,
    .ndo_validate_addr  = eth_validate_addr,
    .ndo_get_stats      = rpmsg_eth_stats,

};

static int rpmsg_eth_probe(struct rpmsg_device *rpdev)
{
    char dummy_payload[] = "dummy_payload";
    struct device *dev = &rpdev->dev;
    struct net_device *netdev;
    struct rpmsg_eth_private *priv;
    char mac[ETH_ALEN] = {0};
    int retval;
  

    mac[ETH_ALEN - 1] = 1;

    netdev = alloc_etherdev(sizeof(struct rpmsg_eth_private));
    if (IS_ERR(netdev))
        return PTR_ERR(netdev);

    netdev->netdev_ops = &netdev_ops;
    netdev->mtu            = MTU_RPMSG;

    strscpy(netdev->name, "rpmsg_net%d", sizeof(netdev->name));

    eth_hw_addr_set(netdev, mac);

    priv = netdev_priv(netdev);

    priv->rpdev = rpdev;
    priv->netdev = netdev;
    INIT_WORK(&priv->immediate, net_xmit_work_handler);
    INIT_DELAYED_WORK(&priv->delayed, net_xmit_delayed_work_handler);
    priv->skb = NULL;
    priv->is_delayed = false;
    spin_lock_init(&priv->shutdown_lock);
    priv->is_shutdown = false;

    dev_set_drvdata(dev, priv);

    // RPMsg remote side expects a fake first packet to learn the master node's return address.
    // The remote end will discard this packet
    retval = rpmsg_send(rpdev->ept, dummy_payload, strlen(dummy_payload));
    if (retval) {
        dev_err(&rpdev->dev, "initial rpmsg_send failed: %d", retval);
        free_netdev(netdev);
        netdev = NULL;
        return retval;
    }

    retval = register_netdev(netdev);
    if (retval) {
        pr_err("ERROR: %s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	return retval;
    }

    return 0;
}

static void rpmsg_eth_remove(struct rpmsg_device *rpdev)
{
    struct rpmsg_eth_private *priv = (struct rpmsg_eth_private*)rpdev->ept->priv;
    unsigned long flags;

    // prevent any in-flight transmissions from being injected into the work queue
    spin_lock_irqsave(&priv->shutdown_lock, flags);
    priv->is_shutdown = true;
    netif_stop_queue(priv->netdev);
    spin_unlock_irqrestore(&priv->shutdown_lock, flags);

    // invariant: since is_shutdown=true and we are on other side of critical section, all work has
    //            already been scheduled, or will not be scheduled (frame dropped).

    // cancel all outstanding scheduled work and/or wait for existing work to finish
    cancel_delayed_work_sync(&priv->delayed);
    cancel_work_sync(&priv->immediate);

    // invariant: there may still be a lingering net_xmit, but it won't start a work queue, and it
    //            won't set priv->skb because is_shutdown is true.

    // free skb, in case it was abandoned by a cancelled work queue request
    if (priv->skb != NULL) {
        dev_consume_skb_any(priv->skb);
        priv->skb = NULL;
    }

    unregister_netdev(priv->netdev);

    free_netdev(priv->netdev);
}

static struct rpmsg_device_id rpmsg_driver_id_table[] = {
    { .name = "rpmsg-eth" },
    { },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_id_table);

static struct rpmsg_driver rpmsg_eth_rpmsg_drv = {
    .drv.name   = KBUILD_MODNAME,
    .id_table   = rpmsg_driver_id_table,
    .probe      = rpmsg_eth_probe,
    .callback   = rpmsg_eth_rx_cb,
    .remove     = rpmsg_eth_remove,
};

static int __init rpmsg_eth_init(void)
{
    int ret;

    ret = register_rpmsg_driver(&rpmsg_eth_rpmsg_drv);
    if (ret < 0) {
        pr_err("Couldn't register driver: %d\n", ret);
        return ret;
    }

    return 0;
}

static void __exit rpmsg_eth_exit(void)
{
    unregister_rpmsg_driver(&rpmsg_eth_rpmsg_drv);
}

module_init(rpmsg_eth_init);
module_exit(rpmsg_eth_exit);

MODULE_AUTHOR("Taras Zaporozhets <zaporozhets.taras@gmail.com>");
MODULE_DESCRIPTION("remote processor networking driver");
MODULE_LICENSE("GPL");
