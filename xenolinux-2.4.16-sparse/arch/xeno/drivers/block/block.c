/******************************************************************************
 * block.c
 * 
 * Virtual block driver for XenoLinux.
 * 
 * adapted from network.c
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>

#include <asm/hypervisor-ifs/block.h>

#ifdef UNDEFINED

#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>

#include <net/sock.h>

#define BLK_TX_IRQ _EVENT_BLK_TX
#define BLK_RX_IRQ _EVENT_BLK_RX

#define TX_MAX_ENTRIES (TX_RING_SIZE - 2)
#define RX_MAX_ENTRIES (RX_RING_SIZE - 2)

#define TX_RING_INC(_i)    (((_i)+1) & (TX_RING_SIZE-1))
#define RX_RING_INC(_i)    (((_i)+1) & (RX_RING_SIZE-1))
#define TX_RING_ADD(_i,_j) (((_i)+(_j)) & (TX_RING_SIZE-1))
#define RX_RING_ADD(_i,_j) (((_i)+(_j)) & (RX_RING_SIZE-1))

#define RX_BUF_SIZE 1600 /* Ethernet MTU + plenty of slack! */



int	    network_probe(struct net_device *dev);
static int  network_open(struct net_device *dev);
static int  network_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int  network_close(struct net_device *dev);
static struct net_device_stats *network_get_stats(struct net_device *dev);
static void network_rx_int(int irq, void *dev_id, struct pt_regs *ptregs);
static void network_tx_int(int irq, void *dev_id, struct pt_regs *ptregs);
static void network_tx_buf_gc(struct net_device *dev);
static void network_alloc_rx_buffers(struct net_device *dev);
static void network_free_rx_buffers(struct net_device *dev);

static struct net_device dev_net_xeno;

/*
 * RX RING:   RX_IDX <= rx_cons <= rx_prod
 * TX RING:   TX_IDX <= tx_cons <= tx_prod
 * (*_IDX allocated privately here, *_cons & *_prod shared with hypervisor)
 */
struct net_private
{
    struct net_device_stats stats;
    struct sk_buff **tx_skb_ring;
    struct sk_buff **rx_skb_ring;
    atomic_t tx_entries;
    unsigned int rx_idx, tx_idx, tx_full;
    net_ring_t *net_ring;
    spinlock_t tx_lock;
};

 
int __init network_probe(struct net_device *dev)
{
    SET_MODULE_OWNER(dev);

    memcpy(dev->dev_addr, "\xFE\xFD\x00\x00\x00\x00", 6);

    dev->open = network_open;
    dev->hard_start_xmit = network_start_xmit;
    dev->stop = network_close;
    dev->get_stats = network_get_stats;

    ether_setup(dev);
    
    return 0;
}


static int network_open(struct net_device *dev)
{
    struct net_private *np;
    int error;

    np = kmalloc(sizeof(struct net_private), GFP_KERNEL);
    if ( np == NULL ) 
    {
        printk(KERN_WARNING "%s: No memory for private data\n", dev->name);
        return -ENOMEM;
    }
    memset(np, 0, sizeof(struct net_private));
    dev->priv = np;

    spin_lock_init(&np->tx_lock);

    atomic_set(&np->tx_entries, 0);

    np->net_ring  = start_info.net_rings;
    np->net_ring->tx_prod = np->net_ring->tx_cons = np->net_ring->tx_event = 0;
    np->net_ring->rx_prod = np->net_ring->rx_cons = np->net_ring->rx_event = 0;
    np->net_ring->tx_ring = NULL;
    np->net_ring->rx_ring = NULL;

    np->tx_skb_ring = kmalloc(TX_RING_SIZE * sizeof(struct sk_buff *),
                              GFP_KERNEL);
    np->rx_skb_ring = kmalloc(RX_RING_SIZE * sizeof(struct sk_buff *),
                              GFP_KERNEL);
    np->net_ring->tx_ring = kmalloc(TX_RING_SIZE * sizeof(tx_entry_t), 
                                  GFP_KERNEL);
    np->net_ring->rx_ring = kmalloc(RX_RING_SIZE * sizeof(rx_entry_t), 
                                  GFP_KERNEL);
    if ( (np->tx_skb_ring == NULL) || (np->rx_skb_ring == NULL) ||
         (np->net_ring->tx_ring == NULL) || (np->net_ring->rx_ring == NULL) )
    {
        printk(KERN_WARNING "%s; Could not allocate ring memory\n", dev->name);
        error = -ENOBUFS;
        goto fail;
    }

    network_alloc_rx_buffers(dev);

    error = request_irq(NET_RX_IRQ, network_rx_int, 0, "net-rx", dev);
    if ( error )
    {
        printk(KERN_WARNING "%s: Could not allocate receive interrupt\n",
               dev->name);
        goto fail;
    }

    error = request_irq(NET_TX_IRQ, network_tx_int, 0, "net-tx", dev);
    if ( error )
    {
        printk(KERN_WARNING "%s: Could not allocate transmit interrupt\n",
               dev->name);
        free_irq(NET_RX_IRQ, dev);
        goto fail;
    }

    printk("XenoLinux Virtual Network Driver installed as %s\n", dev->name);

    netif_start_queue(dev);

    MOD_INC_USE_COUNT;

    return 0;

 fail:
    if ( np->net_ring->rx_ring ) kfree(np->net_ring->rx_ring);
    if ( np->net_ring->tx_ring ) kfree(np->net_ring->tx_ring);
    if ( np->rx_skb_ring ) kfree(np->rx_skb_ring);
    if ( np->tx_skb_ring ) kfree(np->tx_skb_ring);
    kfree(np);
    return error;
}


static void network_tx_buf_gc(struct net_device *dev)
{
    unsigned int i;
    struct net_private *np = dev->priv;
    struct sk_buff *skb;
    unsigned long flags;

    spin_lock_irqsave(&np->tx_lock, flags);

    for ( i = np->tx_idx; i != np->net_ring->tx_cons; i = TX_RING_INC(i) )
    {
        skb = np->tx_skb_ring[i];
        dev_kfree_skb_any(skb);
        atomic_dec(&np->tx_entries);
    }

    np->tx_idx = i;

    if ( np->tx_full && (atomic_read(&np->tx_entries) < TX_MAX_ENTRIES) )
    {
        np->tx_full = 0;
        netif_wake_queue(dev);
    }

    spin_unlock_irqrestore(&np->tx_lock, flags);
}


static void network_alloc_rx_buffers(struct net_device *dev)
{
    unsigned int i;
    struct net_private *np = dev->priv;
    struct sk_buff *skb;
    unsigned int end = RX_RING_ADD(np->rx_idx, RX_MAX_ENTRIES);

    for ( i = np->net_ring->rx_prod; i != end; i = RX_RING_INC(i) )
    {
        skb = dev_alloc_skb(RX_BUF_SIZE);
        if ( skb == NULL ) break;
        skb->dev = dev;
        skb_reserve(skb, 2); /* word align the IP header */
        np->rx_skb_ring[i] = skb;
        np->net_ring->rx_ring[i].addr = (unsigned long)skb->data;
        np->net_ring->rx_ring[i].size = RX_BUF_SIZE - 16; /* arbitrary */
    }

    np->net_ring->rx_prod = i;

    np->net_ring->rx_event = RX_RING_INC(np->rx_idx);

    HYPERVISOR_net_update();
}


static void network_free_rx_buffers(struct net_device *dev)
{
    unsigned int i;
    struct net_private *np = dev->priv;
    struct sk_buff *skb;    

    for ( i = np->rx_idx; i != np->net_ring->rx_prod; i = RX_RING_INC(i) )
    {
        skb = np->rx_skb_ring[i];
        dev_kfree_skb(skb);
    }
}


static int network_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    unsigned int i;
    struct net_private *np = (struct net_private *)dev->priv;

    if ( np->tx_full )
    {
        printk(KERN_WARNING "%s: full queue wasn't stopped!\n", dev->name);
        netif_stop_queue(dev);
        return -ENOBUFS;
    }

    i = np->net_ring->tx_prod;
    np->tx_skb_ring[i] = skb;
    np->net_ring->tx_ring[i].addr = (unsigned long)skb->data;
    np->net_ring->tx_ring[i].size = skb->len;
    np->net_ring->tx_prod = TX_RING_INC(i);
    atomic_inc(&np->tx_entries);

    np->stats.tx_bytes += skb->len;
    np->stats.tx_packets++;

    spin_lock_irq(&np->tx_lock);
    if ( atomic_read(&np->tx_entries) >= TX_MAX_ENTRIES )
    {
        np->tx_full = 1;
        netif_stop_queue(dev);
        np->net_ring->tx_event = TX_RING_ADD(np->tx_idx,
                                           atomic_read(&np->tx_entries) >> 1);
    }
    else
    {
        /* Avoid unnecessary tx interrupts. */
        np->net_ring->tx_event = TX_RING_INC(np->net_ring->tx_prod);
    }
    spin_unlock_irq(&np->tx_lock);

    /* Must do this after setting tx_event: race with updates of tx_cons. */
    network_tx_buf_gc(dev);

    HYPERVISOR_net_update();

    return 0;
}


static void network_rx_int(int irq, void *dev_id, struct pt_regs *ptregs)
{
    unsigned int i;
    struct net_device *dev = (struct net_device *)dev_id;
    struct net_private *np = dev->priv;
    struct sk_buff *skb;
    
 again:
    for ( i = np->rx_idx; i != np->net_ring->rx_cons; i = RX_RING_INC(i) )
    {
        skb = np->rx_skb_ring[i];
        skb_put(skb, np->net_ring->rx_ring[i].size);
        skb->protocol = eth_type_trans(skb, dev);
        np->stats.rx_packets++;
        np->stats.rx_bytes += np->net_ring->rx_ring[i].size;
        netif_rx(skb);
        dev->last_rx = jiffies;
    }

    np->rx_idx = i;

    network_alloc_rx_buffers(dev);
    
    /* Deal with hypervisor racing our resetting of rx_event. */
    smp_mb();
    if ( np->net_ring->rx_cons != i ) goto again;
}


static void network_tx_int(int irq, void *dev_id, struct pt_regs *ptregs)
{
    struct net_device *dev = (struct net_device *)dev_id;
    network_tx_buf_gc(dev);
}


static int network_close(struct net_device *dev)
{
    struct net_private *np = dev->priv;

    netif_stop_queue(dev);
    free_irq(NET_RX_IRQ, dev);
    free_irq(NET_TX_IRQ, dev);
    network_free_rx_buffers(dev);
    kfree(np->net_ring->rx_ring);
    kfree(np->net_ring->tx_ring);
    kfree(np->rx_skb_ring);
    kfree(np->tx_skb_ring);
    kfree(np);
    MOD_DEC_USE_COUNT;
    return 0;
}


static struct net_device_stats *network_get_stats(struct net_device *dev)
{
    struct net_private *np = (struct net_private *)dev->priv;
    return &np->stats;
}


static int __init init_module(void)
{
    memset(&dev_net_xeno, 0, sizeof(dev_net_xeno));
    strcpy(dev_net_xeno.name, "eth%d");
    dev_net_xeno.init = network_probe;
    return (register_netdev(&dev_net_xeno) != 0) ? -EIO : 0;
}


static void __exit cleanup_module(void)
{
    unregister_netdev(&dev_net_xeno);
}

#endif /* UNDEFINED */


static void block_initialize(void)
{
  blk_ring_t *blk_ring = start_info.blk_ring;

  if ( blk_ring == NULL ) return;

  blk_ring->tx_prod = blk_ring->tx_cons = blk_ring->tx_event = 0;
  blk_ring->rx_prod = blk_ring->rx_cons = blk_ring->rx_event = 0;
  blk_ring->tx_ring = NULL;
  blk_ring->rx_ring = NULL;
}


/*
 * block_setup initialized the xeno block device driver
 */

static int __init init_module(void)
{
  block_initialize();
  printk("XenoLinux Virtual Block Device Driver installed\n");
  return 0;
}

static void __exit cleanup_module(void)
{
  printk("XenoLinux Virtual Block Device Driver uninstalled\n");
}

module_init(init_module);
module_exit(cleanup_module);

