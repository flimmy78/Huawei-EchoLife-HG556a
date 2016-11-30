/*
 *	Forwarding database
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	$Id: br_fdb.c,v 1.2 2008/09/22 13:49:41 l65130 Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/times.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <asm/atomic.h>
#include "br_private.h"

static kmem_cache_t *br_fdb_cache;
static int fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		      const unsigned char *addr, int is_local);

void __init br_fdb_init(void)
{
	br_fdb_cache = kmem_cache_create("bridge_fdb_cache",
					 sizeof(struct net_bridge_fdb_entry),
					 0,
					 SLAB_HWCACHE_ALIGN, NULL, NULL);
}

void __exit br_fdb_fini(void)
{
	kmem_cache_destroy(br_fdb_cache);
}


/* if topology_changing then use forward_delay (default 15 sec)
 * otherwise keep longer (default 5 minutes)
 */
static __inline__ unsigned long hold_time(const struct net_bridge *br)
{
	return br->topology_change ? br->forward_delay : br->ageing_time;
}

static __inline__ int has_expired(const struct net_bridge *br,
				  const struct net_bridge_fdb_entry *fdb)
{
	return !fdb->is_static 
		&& time_before_eq(fdb->ageing_timer + hold_time(br), jiffies);
}

static __inline__ int br_mac_hash(const unsigned char *mac)
{
	unsigned long x;

	x = mac[0];
	x = (x << 2) ^ mac[1];
	x = (x << 2) ^ mac[2];
	x = (x << 2) ^ mac[3];
	x = (x << 2) ^ mac[4];
	x = (x << 2) ^ mac[5];

	x ^= x >> 8;

	return x & (BR_HASH_SIZE - 1);
}

static __inline__ void fdb_delete(struct net_bridge_fdb_entry *f)
{
	hlist_del_rcu(&f->hlist);
	if (!f->is_static)
		list_del(&f->u.age_list);

	br_fdb_put(f);
}

/*start of 删除brcm原有snooping中涉及MAC端口转发列表显示、获取、修改、增加的操作函数 by l129990 2008,9,9*/
#if 0
#if defined(CONFIG_MIPS_BRCM)
void dolist(struct net_bridge *br)
{
	struct net_bridge_mc_fdb_entry *dst;
	struct list_head *lh;

	
	if (!br)
	    return;

	printk("bridge	device	group			source			timeout\n");
	list_for_each_rcu(lh, &br->mc_list) {
	    dst = (struct net_bridge_mc_fdb_entry *) list_entry(lh, struct net_bridge_mc_fdb_entry, list);
	    /* Start of igmp snooping by f60014464 20060628 */
	    if (NULL == dst->dst)
	    {
	        continue;
	    }
	    /* End of igmp snooping by f60014464 20060628 */
	    printk("%s	%s  	", br->dev->name, dst->dst->dev->name);
	    addr_debug((unsigned char *) &dst->addr);
	    printk("	");
	    addr_debug((unsigned char *) &dst->host);
	    printk("	%d\n", (int) (dst->tstamp - jiffies)/HZ);
	}
}

int br_mc_fdb_update(struct net_bridge *br, struct net_bridge_port *prt, unsigned char *dest, unsigned char *host)
{
	struct net_bridge_mc_fdb_entry *dst;
	struct list_head *lh;
	int ret = 0;

    /*START MODIFY:Jaffen for A36D03767 2007-07-17*/
	list_for_each_rcu(lh, &br->mc_list) {
	    dst = (struct net_bridge_mc_fdb_entry *) list_entry(lh, struct net_bridge_mc_fdb_entry, list);
	    if (!memcmp(&dst->addr, dest, ETH_ALEN)){
    		if ((!memcmp(&dst->host, host, ETH_ALEN)) && (dst->dst == prt)){    		
                dst->tstamp = jiffies + QUERY_TIMEOUT*HZ;
    		    ret = 1;
    		}
	    }
	}
    /*END MODIFY:Jaffen for A36D03767 2007-07-17*/
	
	return ret;
}


struct net_bridge_mc_fdb_entry *br_mc_fdb_get(struct net_bridge *br, struct net_bridge_port *prt, unsigned char *dest, unsigned char *host)
{
	struct net_bridge_mc_fdb_entry *dst;
	struct list_head *lh;
    
	list_for_each_rcu(lh, &br->mc_list) {
	    dst = (struct net_bridge_mc_fdb_entry *) list_entry(lh, struct net_bridge_mc_fdb_entry, list);
	    if ((!memcmp(&dst->addr, dest, ETH_ALEN)) && (!memcmp(&dst->host, host, ETH_ALEN))) {
		if (dst->dst == prt)
		    return dst;
	    }
	}
	
	return NULL;
}

extern mac_addr upnp_addr;

int br_mc_fdb_add(struct net_bridge *br, struct net_bridge_port *prt, unsigned char *dest, unsigned char *host)
{
	struct net_bridge_mc_fdb_entry *mc_fdb;

	//printk("--- add mc entry ---\n");

	if (!memcmp(dest, &upnp_addr, ETH_ALEN))
	    return 0;
	    
	if (br_mc_fdb_update(br, prt, dest, host))
	    return 0;
	    
	mc_fdb = kmalloc(sizeof(struct net_bridge_mc_fdb_entry), GFP_KERNEL);
	if (!mc_fdb)
	    return ENOMEM;
	memcpy(mc_fdb->addr.addr, dest, ETH_ALEN);
	memcpy(mc_fdb->host.addr, host, ETH_ALEN);
	mc_fdb->dst = prt;
	mc_fdb->tstamp = jiffies + QUERY_TIMEOUT*HZ;;
	spin_lock_bh(&br->mcl_lock);
	list_add_tail_rcu(&mc_fdb->list, &br->mc_list);
	spin_unlock_bh(&br->mcl_lock);

	if (!br->start_timer) {
    	    init_timer(&br->igmp_timer);
	    br->igmp_timer.expires = jiffies + TIMER_CHECK_TIMEOUT*HZ;
	    br->igmp_timer.function = query_timeout;
	    br->igmp_timer.data = (unsigned long) br;
	    add_timer(&br->igmp_timer);
	    br->start_timer = 1;
	}

	return 1;
}
#endif
#endif
/*end of 删除brcm原有snooping中涉及MAC端口转发列表显示、获取、修改、增加的操作函数 by l129990 2008,9,9*/

/*start of 删除brcm原有的清空桥的mc_list列表函数 by l129990 2008,9,6*/
#if 0
void br_mc_fdb_cleanup(struct net_bridge *br)
{
	struct net_bridge_mc_fdb_entry *dst;
	struct list_head *lh;
	struct list_head *tmp;
    
	spin_lock_bh(&br->mcl_lock);
	list_for_each_safe_rcu(lh, tmp, &br->mc_list) {
	    dst = (struct net_bridge_mc_fdb_entry *) list_entry(lh, struct net_bridge_mc_fdb_entry, list);
	    list_del_rcu(&dst->list);
	    kfree(dst);
	}
	/*start of WAN <3.5.1删除桥未删除igmp定时器，导致超时执行错误> porting by s60000658 20060504*/
	del_timer(&(br->igmp_timer));
	/*end of WAN <3.5.1删除桥未删除igmp定时器，导致超时执行错误> porting by s60000658 20060504*/
	spin_unlock_bh(&br->mcl_lock);
}
#endif
/*end of 删除brcm原有的清空桥的mc_list列表函数 by l129990 2008,9,6*/

/*start of 删除brcm原有的snooping中清除某个端口的MAC端口转发表项的函数 by l129990 2008,9,9*/
#if 0
void br_mc_fdb_remove_grp(struct net_bridge *br, struct net_bridge_port *prt, unsigned char *dest)
{
	struct net_bridge_mc_fdb_entry *dst;
	struct list_head *lh;
	struct list_head *tmp;

	spin_lock_bh(&br->mcl_lock);
	list_for_each_safe_rcu(lh, tmp, &br->mc_list) {
	    dst = (struct net_bridge_mc_fdb_entry *) list_entry(lh, struct net_bridge_mc_fdb_entry, list);
	    if ((!memcmp(&dst->addr, dest, ETH_ALEN)) && (dst->dst == prt)) {
		list_del_rcu(&dst->list);
		kfree(dst);
	    }
	}
	spin_unlock_bh(&br->mcl_lock);
}


int br_mc_fdb_remove(struct net_bridge *br, struct net_bridge_port *prt, unsigned char *dest, unsigned char *host)
{
	struct net_bridge_mc_fdb_entry *mc_fdb;

	//printk("--- remove mc entry ---\n");
	
	if ((mc_fdb = br_mc_fdb_get(br, prt, dest, host))) {
	    spin_lock_bh(&br->mcl_lock);
	    list_del_rcu(&mc_fdb->list);
	    kfree(mc_fdb);
	    spin_unlock_bh(&br->mcl_lock);

	    return 1;
	}
	
	return 0;
}
#endif
/*end of 删除brcm原有的snooping中清除某个端口的MAC端口转发表项的函数 by l129990 2008,9,9*/

void br_fdb_changeaddr(struct net_bridge_port *p, const unsigned char *newaddr)
{
	struct net_bridge *br = p->br;
	int i;
	
	spin_lock_bh(&br->hash_lock);

	/* Search all chains since old address/hash is unknown */
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct hlist_node *h;
		hlist_for_each(h, &br->hash[i]) {
			struct net_bridge_fdb_entry *f;

			f = hlist_entry(h, struct net_bridge_fdb_entry, hlist);
			if (f->dst == p && f->is_local) {
				/* maybe another port has same hw addr? */
				struct net_bridge_port *op;
				list_for_each_entry(op, &br->port_list, list) {
					if (op != p && 
					    !memcmp(op->dev->dev_addr,
						    f->addr.addr, ETH_ALEN)) {
						f->dst = op;
						goto insert;
					}
				}

				/* delete old one */
				fdb_delete(f);
				goto insert;
			}
		}
	}
 insert:
	/* insert new address,  may fail if invalid address or dup. */
	fdb_insert(br, p, newaddr, 1);


	spin_unlock_bh(&br->hash_lock);
}

void br_fdb_cleanup(unsigned long _data)
{
	struct net_bridge *br = (struct net_bridge *)_data;
	struct list_head *l, *n;
	unsigned long delay;

	spin_lock_bh(&br->hash_lock);
	delay = hold_time(br);

	list_for_each_safe(l, n, &br->age_list) {
		struct net_bridge_fdb_entry *f;
		unsigned long expires;

		f = list_entry(l, struct net_bridge_fdb_entry, u.age_list);
		expires = f->ageing_timer + delay;

		if (time_before_eq(expires, jiffies)) {
			WARN_ON(f->is_static);
			pr_debug("expire age %lu jiffies %lu\n",
				 f->ageing_timer, jiffies);
			fdb_delete(f);
		} else {
			mod_timer(&br->gc_timer, expires);
			break;
		}
	}
	spin_unlock_bh(&br->hash_lock);
}

void br_fdb_delete_by_port(struct net_bridge *br, struct net_bridge_port *p)
{
	int i;

	spin_lock_bh(&br->hash_lock);
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct hlist_node *h, *g;
		
		hlist_for_each_safe(h, g, &br->hash[i]) {
			struct net_bridge_fdb_entry *f
				= hlist_entry(h, struct net_bridge_fdb_entry, hlist);
			if (f->dst != p) 
				continue;

			/*
			 * if multiple ports all have the same device address
			 * then when one port is deleted, assign
			 * the local entry to other port
			 */
			if (f->is_local) {
				struct net_bridge_port *op;
				list_for_each_entry(op, &br->port_list, list) {
					if (op != p && 
					    !memcmp(op->dev->dev_addr,
						    f->addr.addr, ETH_ALEN)) {
						f->dst = op;
						goto skip_delete;
					}
				}
			}

			fdb_delete(f);
		skip_delete: ;
		}
	}
	spin_unlock_bh(&br->hash_lock);
}

/* No locking or refcounting, assumes caller has no preempt (rcu_read_lock) */
struct net_bridge_fdb_entry *__br_fdb_get(struct net_bridge *br,
					  const unsigned char *addr)
{
	struct hlist_node *h;
	struct net_bridge_fdb_entry *fdb;

	hlist_for_each_entry_rcu(fdb, h, &br->hash[br_mac_hash(addr)], hlist) {
		if (!memcmp(fdb->addr.addr, addr, ETH_ALEN)) {
			if (unlikely(has_expired(br, fdb)))
				break;
			return fdb;
		}
	}

	return NULL;
}

/* Interface used by ATM hook that keeps a ref count */
struct net_bridge_fdb_entry *br_fdb_get(struct net_bridge *br, 
					unsigned char *addr)
{
	struct net_bridge_fdb_entry *fdb;

	rcu_read_lock();
	fdb = __br_fdb_get(br, addr);
	if (fdb) 
		atomic_inc(&fdb->use_count);
	rcu_read_unlock();
	return fdb;
}

static void fdb_rcu_free(struct rcu_head *head)
{
	struct net_bridge_fdb_entry *ent
		= container_of(head, struct net_bridge_fdb_entry, u.rcu);
	kmem_cache_free(br_fdb_cache, ent);
}

/* Set entry up for deletion with RCU  */
void br_fdb_put(struct net_bridge_fdb_entry *ent)
{
	if (atomic_dec_and_test(&ent->use_count))
		call_rcu(&ent->u.rcu, fdb_rcu_free);
}

/*
 * Fill buffer with forwarding table records in 
 * the API format.
 */
int br_fdb_fillbuf(struct net_bridge *br, void *buf,
		   unsigned long maxnum, unsigned long skip)
{
	struct __fdb_entry *fe = buf;
	int i, num = 0;
	struct hlist_node *h;
	struct net_bridge_fdb_entry *f;

	memset(buf, 0, maxnum*sizeof(struct __fdb_entry));

	rcu_read_lock();
	for (i = 0; i < BR_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(f, h, &br->hash[i], hlist) {
			if (num >= maxnum)
				goto out;

			if (has_expired(br, f)) 
				continue;

			if (skip) {
				--skip;
				continue;
			}

			/* convert from internal format to API */
			memcpy(fe->mac_addr, f->addr.addr, ETH_ALEN);
			fe->port_no = f->dst->port_no;
			fe->is_local = f->is_local;
			if (!f->is_static)
				fe->ageing_timer_value = jiffies_to_clock_t(jiffies - f->ageing_timer);
			++fe;
			++num;
		}
	}

 out:
	rcu_read_unlock();

	return num;
}

static int fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		  const unsigned char *addr, int is_local)
{
	struct hlist_node *h;
	struct net_bridge_fdb_entry *fdb;
	int hash = br_mac_hash(addr);

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	hlist_for_each_entry(fdb, h, &br->hash[hash], hlist) {
		if (!memcmp(fdb->addr.addr, addr, ETH_ALEN)) {
			/* attempt to update an entry for a local interface */
			if (fdb->is_local) {
				/* it is okay to have multiple ports with same 
				 * address, just don't allow to be spoofed.
				 */
				if (is_local) 
					return 0;
					
#if 0			/* Begin Broadcom commented out code */
				 
				if (net_ratelimit()) 
					printk(KERN_WARNING "%s: received packet with "
					       " own address as source address\n",
					       source->dev->name);
					       
#endif			/* End Broadcom commented out code */
				return -EEXIST;
			}

			if (is_local) {
				printk(KERN_WARNING "%s adding interface with same address "
				       "as a received packet\n",
				       source->dev->name);
				goto update;
			}

			if (fdb->is_static)
				return 0;

			/* move to end of age list */
			list_del(&fdb->u.age_list);
			goto update;
		}
	}

	fdb = kmem_cache_alloc(br_fdb_cache, GFP_ATOMIC);
	if (!fdb)
		return ENOMEM;

	memcpy(fdb->addr.addr, addr, ETH_ALEN);
	atomic_set(&fdb->use_count, 1);
	hlist_add_head_rcu(&fdb->hlist, &br->hash[hash]);

	if (!timer_pending(&br->gc_timer)) {
		br->gc_timer.expires = jiffies + hold_time(br);
		add_timer(&br->gc_timer);
	}

 update:
	fdb->dst = source;
	fdb->is_local = is_local;
	fdb->is_static = is_local;
	fdb->ageing_timer = jiffies;
	if (!is_local) 
		list_add_tail(&fdb->u.age_list, &br->age_list);

	return 0;
}

int br_fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		  const unsigned char *addr, int is_local)
{
	int ret;

	spin_lock_bh(&br->hash_lock);
	ret = fdb_insert(br, source, addr, is_local);
	spin_unlock_bh(&br->hash_lock);
	return ret;
}
