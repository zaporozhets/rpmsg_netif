/* 2024, Taras Zaporozhets */

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
};

static netdev_tx_t rpmsg_netdev_xmit(struct sk_buff *skb,
                            struct net_device *dev)
{
    struct rpmsg_eth_private *priv = netdev_priv(dev);
    int ret;

    ret = rpmsg_send(priv->rpdev->ept, skb->data, skb->len);
    if (ret) {
        dev_err(&priv->rpdev->dev, "rpmsg_send failed: %d\n", ret);
    }

    dev_kfree_skb_any(skb);

    priv->stats.tx_packets++;
    priv->stats.tx_bytes += skb->len;

    return NETDEV_TX_OK;
}

static int rpmsg_netdev_open(struct net_device *ndev)
{
    netif_start_queue(ndev);
    return 0;
}

int rpmsg_netdev_stop (struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}


struct net_device_stats *rpmsg_netdev_stats(struct net_device *dev)
{
    struct rpmsg_eth_private *priv = netdev_priv(dev);
    return &priv->stats;
}

static int rpmsg_netdev_rx_cb(struct rpmsg_device *rpdev, void *data, int len, void *drv_priv, u32 src)
{
    struct rpmsg_eth_private *priv = dev_get_drvdata(&rpdev->dev);
    struct sk_buff *skb;

    skb = dev_alloc_skb(len);

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
    .ndo_open           = rpmsg_netdev_open,
    .ndo_stop           = rpmsg_netdev_stop,
    .ndo_start_xmit     = rpmsg_netdev_xmit,
    .ndo_validate_addr  = eth_validate_addr,
    .ndo_get_stats      = rpmsg_netdev_stats,

};

static int rpmsg_netdev_probe(struct rpmsg_device *rpdev)
{
    struct device *dev = &rpdev->dev;
    struct net_device *netdev;
    int retval;

    struct rpmsg_eth_private *priv;
    char mac[ETH_ALEN] = {0};

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

    dev_set_drvdata(dev, priv);

    retval = register_netdev(netdev);
    if (retval) {
        pr_err("ERROR: %s %s %d\n", __FILE__, __FUNCTION__, __LINE__);

    }

    return 0;
}

static void rpmsg_netdev_remove(struct rpmsg_device *rpdev)
{
    struct rpmsg_eth_private *priv = dev_get_drvdata(&rpdev->dev);
    unregister_netdev(priv->netdev);
    free_netdev(priv->netdev);
}

static struct rpmsg_device_id rpmsg_driver_id_table[] = {
    { .name = "rpmsg-eth" },
    { },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_id_table);

static struct rpmsg_driver rpmsg_netdev_rpmsg_drv = {
    .drv.name   = KBUILD_MODNAME,
    .id_table   = rpmsg_driver_id_table,
    .probe      = rpmsg_netdev_probe,
    .callback   = rpmsg_netdev_rx_cb,
    .remove     = rpmsg_netdev_remove,
};

static int __init rpmsg_netdev_init(void)
{
    int ret;

    ret = register_rpmsg_driver(&rpmsg_netdev_rpmsg_drv);
    if (ret < 0) {
        pr_err("Couldn't register driver: %d\n", ret);
        return ret;
    }

    return 0;
}

static void __exit rpmsg_netdev_exit(void)
{
    unregister_rpmsg_driver(&rpmsg_netdev_rpmsg_drv);
}

module_init(rpmsg_netdev_init);
module_exit(rpmsg_netdev_exit);

MODULE_AUTHOR("Taras Zaporozhets <zaporozhets.taras@gmail.com>");
MODULE_DESCRIPTION("remote processor networking driver");
MODULE_LICENSE("GPL v2");
