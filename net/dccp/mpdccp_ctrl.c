/*  SPDX-License-Identifier: GNU General Public License v2 only (GPL-2.0-only)
 *
 * Copyright (C) 2017 by Andreas Philipp Matz, Deutsche Telekom AG
 * Copyright (C) 2017 by Markus Amend, Deutsche Telekom AG
 * Copyright (C) 2020 by Nathalie Romo, Deutsche Telekom AG
 * Copyright (C) 2020 by Frank Reker, Deutsche Telekom AG
 * Copyright (C) 2021 by Romeo Cane, Deutsche Telekom AG
 *
 * MPDCCP - DCCP bundling kernel module
 *
 * This module implements a bundling mechanism that aggregates
 * multiple paths using the DCCP protocol.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/dccp.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <uapi/linux/net.h>

#include <net/inet_common.h>
#include <net/inet_sock.h>
#include <net/protocol.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <linux/inetdevice.h>
#include <net/mpdccp_link.h>
#include <asm/unaligned.h>

#include "mpdccp.h"
#include "mpdccp_scheduler.h"
#include "mpdccp_reordering.h"
#include "mpdccp_pm.h"

/*
 * TODO GLOBAL
 * 4)   CLEANUP TODO: Free structures, close sockets etc on module exit to avoid crash
 * 5)   Something like using tcp_read_sock() instead of kernel_recvmsg() would be nice; however, there is 
 *      currently no such function for DCCP. (about 2% performance gain in TCP)
 * 8)   Implement fragmentation (unsuppported by Linux DCCP stack)
 * 9)   Implement service code on both sides (no meaningful service codes are defined by IANA)
 * 12)  Implement data sequence numbers in a new MPDCCP option
 * KNOWN BUGS
 * 1)   For now, this requires CONFIG_NET_L3_MASTER_DEV disabled. When enabled, compute_score() in inet_hashtables.c
 *      will return -1 as exact_dif==true and sk->sk_bound_dev_if != dif (first is 0/not set). Maybe I need to set 
 *      sk->sk_bound_dev somewhere? I dont know if this is going to bite me. 
 *      More info at http://netdevconf.org/1.2/papers/ahern-what-is-l3mdev-paper.pdf
 * 2)   DCCP in the Linux kernel does not support fragmentation and neither does this bundling solution.
 */


#define MPDCCP_SERVER_BACKLOG	1000
#define DUMP_PACKET_CONTENT	0

static struct kmem_cache *mpdccp_cb_cache __read_mostly;


/* A list of all MPDCCP connections (represented as mpcb's) */
struct list_head __rcu pconnection_list;
EXPORT_SYMBOL(pconnection_list);

spinlock_t pconnection_list_lock;
EXPORT_SYMBOL(pconnection_list_lock);

static struct kmem_cache *mpdccp_mysock_cache __read_mostly;

/* Work queue for all reading and writing to/from the socket. */
struct workqueue_struct *mpdccp_wq;
EXPORT_SYMBOL(mpdccp_wq);

/*********************************************************
 * Work queue functions
 *********************************************************/

/*
 * Atomically queue work on a connection after the specified delay.
 * Returns 0 if work was queued, or an error code otherwise.
 */
//static int queue_bnd_work_delay(struct sock *sk, unsigned long delay)
//{
//    struct my_sock *my_sk = mpdccp_my_sock(sk);
//
//    if (!queue_delayed_work(mpdccp_wq, &my_sk->work, delay)) {
//        mpdccp_pr_debug("%p - already queued, wq: %p, work: %p, delay: %lu \n", sk, mpdccp_wq, &my_sk->work, delay);
//        return -EBUSY;
//    }
//
//    mpdccp_pr_debug("queue ok %p wq: %p, work: %p delay: %lu\n", sk, mpdccp_wq, &my_sk->work, delay);
//    return 0;
//}

//static int queue_bnd_work(struct sock *sk)
//{
//    return queue_bnd_work_delay(sk, 0);
//}

//static void mpdccp_cancel_bnd_work(struct sock *sk)
//{
//    struct my_sock *my_sk = mpdccp_my_sock(sk);
//
//    if (cancel_delayed_work(&my_sk->work)) {
//        mpdccp_pr_debug(" Work %p cancelled\n", sk);
//    }
//}

void mpdccp_wq_flush(void)
{
    mpdccp_pr_debug("in mpdccp_wq_flush");
    flush_workqueue(mpdccp_wq);
}

static int mpdccp_read_from_subflow (struct sock *sk)
{
    int peeked, sz, ret, off=0;
    struct sk_buff *skb = NULL;
    struct my_sock *my_sk = mpdccp_my_sock(sk);
    struct mpdccp_cb *mpcb = my_sk->mpcb;
    const struct dccp_hdr *dh;

    if(!sk)
        return -EINVAL;

    skb = __skb_recv_datagram (sk, MSG_DONTWAIT, NULL, &peeked, &off, &ret);
    if (!skb) 
        return 0;

    sz = skb->len;
    dh = dccp_hdr(skb);

    switch(dh->dccph_type) {
        case DCCP_PKT_DATA:
        case DCCP_PKT_DATAACK:
            if (sz > 0) {
                /* Forward skb to reordering engine */
                mpcb->reorder_ops->do_reorder(mpdccp_init_rcv_buff(sk, skb, mpcb));
                mpdccp_pr_debug("Read %d bytes from socket %p.\n", sz, sk);
            } else {
                mpdccp_pr_debug("Read zero-length data from socket %p, discarding\n", sk);
                __kfree_skb(skb);
            }
            break;
        case DCCP_PKT_CLOSE:
        case DCCP_PKT_CLOSEREQ:
            mpdccp_my_sock(sk)->closing = 1;
            schedule_work(&mpdccp_my_sock(sk)->close_work);
	        __kfree_skb(skb);
            break;
        default:
            mpdccp_pr_debug("unhandled packet type %d from socket %p", dh->dccph_type, sk);
            sz = -1;
    }
    return sz;
}

int mpdccp_forward_skb(struct sk_buff *skb, struct mpdccp_cb *mpcb)
{
	struct sock	*meta_sk;
	int ret;
	mpdccp_pr_debug("forward packet\n");
	if (!skb) return -EINVAL;
	if (!mpcb || !mpcb->meta_sk) {
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}
	meta_sk = mpcb->meta_sk;
	/* we should have a separate setting for rx_qlen, for now use tx_qlen */
	if (dccp_sk(meta_sk)->dccps_tx_qlen &&
			meta_sk->sk_receive_queue.qlen >= dccp_sk(meta_sk)->dccps_tx_qlen) {
		/* drop packet - FIXME: differ between drop oldest and drop newest */
		//mpdccp_pr_debug ("drop packet - queue full\n");
		printk ("mpdccp_forward_skb: drop packet - queue full\n");
		dev_kfree_skb_any(skb);
		return -ENOBUFS;
	}

	mpdccp_pr_debug ("enqueue packet\n");
	ret = sock_queue_rcv_skb(meta_sk, skb);
	if (ret < 0) {
		/*
		 * shouldn't happen
		 */
		printk(KERN_ERR "%s: sock_queue_rcv_skb failed! err %d bufsize %d\n",
		       __func__, ret, meta_sk->sk_rcvbuf);
		dev_kfree_skb_any(skb);
	}
#if 0
	__skb_queue_tail(&meta_sk->sk_receive_queue, skb);
	skb_set_owner_r(skb, meta_sk);
	if (meta_sk->sk_data_ready) meta_sk->sk_data_ready(meta_sk);
#endif

	return 0;
}
EXPORT_SYMBOL(mpdccp_forward_skb);


/* *********************************
 * mpcb related functions
 * *********************************/

struct mpdccp_cb *mpdccp_alloc_mpcb(void)
{
    //int i;
    struct mpdccp_cb *mpcb = NULL;

    /* Allocate memory for mpcb */
    mpcb = kmem_cache_zalloc(mpdccp_cb_cache, GFP_ATOMIC);
    if (!mpcb) {
        mpdccp_pr_debug("Failed to initialize mpcb.\n");
        return NULL;
    }

    /* No locking needed, as nobody can access the struct yet */
    INIT_LIST_HEAD(&mpcb->psubflow_list);
    INIT_LIST_HEAD(&mpcb->plisten_list);
    INIT_LIST_HEAD(&mpcb->prequest_list);
    spin_lock_init(&mpcb->psubflow_list_lock);
    spin_lock_init(&mpcb->plisten_list_lock);

    mpcb->cnt_subflows      = 0;
    mpcb->multipath_active  = 1;     //socket option; always active for now
    mpcb->dsn_local  = 0;
    mpcb->dsn_remote = 0;

    /* TODO: move the two settings below to sysctl controls */
    mpcb->mpdccp_suppkeys = MPDCCP_SUPPKEYS;
    mpcb->mpdccp_ver = MPDCCP_VERSION_NUM;
    mpcb->glob_lfor_seqno = GLOB_SEQNO_INIT;
    mpcb->mp_oall_seqno = GLOB_SEQNO_INIT;

    mpdccp_init_path_manager(mpcb);
    mpdccp_init_scheduler(mpcb);
    mpdccp_init_reordering(mpcb);

    spin_lock_bh(&pconnection_list_lock);
    list_add_tail_rcu(&mpcb->connection_list, &pconnection_list);
    mpdccp_pr_debug("Added new entry to pconnection_list @ %p\n", mpcb);
    spin_unlock_bh(&pconnection_list_lock);

    mpdccp_pr_debug("Sucessfully initialized mpcb at %p.\n", mpcb);
    
    return mpcb;
}

int mpdccp_destroy_mpcb(struct mpdccp_cb *mpcb)
{
	struct sock	*sk;
	int		ret;
	struct list_head *pos, *temp;
	struct my_sock *mysk;

	if (!mpcb) return -EINVAL;

	/* Delete the mpcb from the list of MPDCCP connections */
	spin_lock(&pconnection_list_lock);
	list_del_rcu(&mpcb->connection_list);
	spin_unlock(&pconnection_list_lock);
	
	/* close all subflows */
	list_for_each_safe(pos, temp, &((mpcb)->psubflow_list)) {
		mysk = list_entry(pos, struct my_sock, sk_list);
		if (mysk) {
			sk = mysk->my_sk_sock;
			ret = mpdccp_close_subflow(mpcb, sk, 1);
			if (ret < 0) {
				mpdccp_pr_debug ("error closing subflow: %d\n", ret);
				return ret;
			}
		}
	}

	/* close all listening sockets */
	list_for_each_safe(pos, temp, &((mpcb)->plisten_list)) {
		mysk = list_entry(pos, struct my_sock, sk_list);
		if (mysk) {
			sk = mysk->my_sk_sock;	
			ret = mpdccp_close_subflow(mpcb, sk, 1);
			if (ret < 0) {
				mpdccp_pr_debug ("error closing listen socket: %d\n", ret);
				return ret;
			}
		}
	}

	/* close all request sockets */
	list_for_each_safe(pos, temp, &((mpcb)->prequest_list)) {
		mysk = list_entry(pos, struct my_sock, sk_list);
		if (mysk) {
			sk = mysk->my_sk_sock;
			ret = mpdccp_close_subflow(mpcb, sk, 1);
			if (ret < 0) {
				mpdccp_pr_debug ("error closing request socket: %d\n", ret);
				return ret;
			}
		}
	}

	mpdccp_cleanup_reordering(mpcb);
	mpdccp_cleanup_scheduler(mpcb);
	mpdccp_cleanup_path_manager(mpcb);
   
	/* Wait for all readers to finish before removal */
	//TAG-0.7: synchronize_rcu() is blocking. Migrate this to void call_rcu(struct rcu_head *head, rcu_callback_t func);
	//Do not call - we are atomic!
	//synchronize_rcu();
	kmem_cache_free(mpdccp_cb_cache, mpcb);
	
	return 0;
}



/******************************************************
 * 'mysock' custom functions
 ******************************************************/
int listen_backlog_rcv (struct sock *sk, struct sk_buff *skb);
void listen_data_ready (struct sock *sk);
void mp_state_change (struct sock *sk);

/* TODO: Differentiate between SUBFLOW and LISTEN socket list!!!
 * Free additional structures and call sk_destruct on the socket*/
void my_sock_destruct (struct sock *sk)
{
    struct my_sock   *my_sk = mpdccp_my_sock(sk);
    struct mpdccp_cb *mpcb  = my_sk->mpcb;

    mpdccp_report_destroy (sk);

    /* Delete this subflow from the list of mpcb subflows */
    if (!list_empty(&my_sk->sk_list)) {
        spin_lock(&mpcb->psubflow_list_lock);
        list_del_rcu(&my_sk->sk_list);
        if (my_sk->link_info) {
            mpdccp_link_put (my_sk->link_info);
            my_sk->link_info = NULL;
        }
        mpcb->cnt_subflows--;
        spin_unlock(&mpcb->psubflow_list_lock);
    }

    /* Wait for all readers to finish before removal */
    //TAG-0.7: synchronize_rcu() is blocking. Migrate this to void call_rcu(struct rcu_head *head, rcu_callback_t func);
    // atomic -> don't sync
    // synchronize_rcu();

    sk->sk_user_data = NULL;

    /* Restore old function pointers */
    if(my_sk->sk_data_ready)
        sk->sk_data_ready       = my_sk->sk_data_ready;

    if(my_sk->sk_backlog_rcv)
        sk->sk_backlog_rcv      = my_sk->sk_backlog_rcv;

    if(my_sk->sk_destruct)
        sk->sk_destruct         = my_sk->sk_destruct;

    if(my_sk->sk_state_change)
        sk->sk_state_change     = my_sk->sk_state_change;

    if (my_sk->pcb)
        mpdccp_free_reorder_path_cb(my_sk->pcb);

    kmem_cache_free(mpdccp_mysock_cache, my_sk);

    if (sk->sk_destruct) 
        sk->sk_destruct (sk);

    module_put(THIS_MODULE);

    mpdccp_pr_debug ("subflow %p removed from mpcb %p, remaining subflows: %d", sk, mpcb, mpcb->cnt_subflows);
    if ((mpcb->cnt_subflows == 0) && (mpcb->meta_sk->sk_state != DCCP_CLOSED)) {
        mpdccp_pr_debug ("closing meta %p\n", mpcb->meta_sk);
        dccp_done(mpcb->meta_sk);
    }
}

static void mpdccp_close_worker(struct work_struct *work)
{
    struct my_sock *my_sk;
    struct sock *sk;

    my_sk = container_of(work, struct my_sock, close_work);
    sk = my_sk->my_sk_sock;

    /* Finish the passive close stuff before final closure */
    if ((sk->sk_state == DCCP_PASSIVE_CLOSE)
        || (sk->sk_state == DCCP_PASSIVE_CLOSEREQ)) {
            dccp_finish_passive_close(sk);
    }

    dccp_close(sk, 0);
}

int my_sock_init (struct sock *sk, struct mpdccp_cb *mpcb, int if_idx, enum mpdccp_role role)
{
    struct my_sock *my_sk;
    mpdccp_pr_debug("Enter my_sock_init().\n");
    my_sk = kmem_cache_zalloc(mpdccp_mysock_cache, GFP_ATOMIC);
    if (!my_sk)
        return -ENOBUFS;

    /* No locking needed, as readers do not yet have access to the structure */
    INIT_LIST_HEAD(&my_sk->sk_list);
    my_sk->my_sk_sock   = sk;
    my_sk->mpcb         = mpcb;
    my_sk->if_idx       = if_idx;
    my_sk->pcb          = NULL;

    /* Init private scheduler data */
    memset(my_sk->sched_priv, 0, MPDCCP_SCHED_SIZE);

    /* Save the original socket callbacks before rewriting */

	mpdccp_pr_debug("role %d my_sk %p", role, my_sk);
	my_sk->sk_data_ready    = sk->sk_data_ready;
	my_sk->sk_backlog_rcv   = sk->sk_backlog_rcv;
	my_sk->sk_destruct      = sk->sk_destruct;


    sk->sk_data_ready       = listen_data_ready;
    sk->sk_backlog_rcv      = listen_backlog_rcv;
    sk->sk_destruct         = my_sock_destruct;

    if (role == MPDCCP_CLIENT) {
        my_sk->sk_state_change  = sk->sk_state_change;
        sk->sk_state_change     = mp_state_change;
    }

    mpdccp_pr_debug("role %d sk %p kex_done %d", role, sk, mpcb->kex_done);
    if (!mpcb->kex_done) {
        /* Set the kex flag for this socket if no key exchange has been done before */
        dccp_sk(sk)->is_kex_sk = 1;
    } else  {
        if (role == MPDCCP_CLIENT) {
            /* Generate local nonce */
            get_random_bytes(&dccp_sk(sk)->mpdccp_loc_nonce, 4);
            mpdccp_pr_debug("client: generated nonce %x\n", dccp_sk(sk)->mpdccp_loc_nonce);
        }
    }

    // Memory is already reserved in struct my_sk
    mpdccp_pr_debug("Entered my_sock_init\n");

    sk->sk_user_data        = my_sk;

    INIT_WORK(&my_sk->close_work, mpdccp_close_worker);
    /* Make sure refcount for this module is increased */
    try_module_get(THIS_MODULE);
    return 0;
}
EXPORT_SYMBOL_GPL(my_sock_init);


int
mpdccp_ctrl_maycpylink (struct sock *sk)
{
    struct my_sock 		*my_sk;
    struct mpdccp_link_info	*link, *oldlink;
    int				ret;

    if (!sk) return -EINVAL;
    my_sk = mpdccp_my_sock (sk);
    if (!my_sk) return -EINVAL;	/* no mpdccp socket */
    if (my_sk->link_iscpy) return 0;	/* already copied */
    ret = mpdccp_link_copy (&link, my_sk->link_info);
    if (ret < 0) {
	mpdccp_pr_error ("cannot copy link_info: %d", ret);
	return ret;
    }
    rcu_read_lock ();
    oldlink = xchg ((__force struct mpdccp_link_info **)&my_sk->link_info, link);
    rcu_read_unlock ();
    mpdccp_link_put (oldlink);
    return 0;
}
EXPORT_SYMBOL (mpdccp_ctrl_maycpylink);

struct mpdccp_link_info*
mpdccp_ctrl_getlink (struct sock *sk)
{
    struct my_sock 		*my_sk;
    struct mpdccp_link_info	*link;

    if (!sk) return NULL;
    my_sk = mpdccp_my_sock (sk);
    if (!my_sk) return NULL;	/* no mpdccp socket */
    rcu_read_lock();
    link = my_sk->link_info;
    mpdccp_link_get (link);
    rcu_read_unlock();
    return link;
}
EXPORT_SYMBOL (mpdccp_ctrl_getlink);

struct mpdccp_link_info*
mpdccp_ctrl_getcpylink (struct sock *sk)
{
    int				ret;
    struct mpdccp_link_info	*link;

    rcu_read_lock();
    ret = mpdccp_ctrl_maycpylink (sk);
    if (ret < 0) {
	rcu_read_unlock();
	return NULL;
    }
    link = mpdccp_ctrl_getlink (sk);
    rcu_read_unlock();
    return link;
}
EXPORT_SYMBOL (mpdccp_ctrl_getcpylink);

int
mpdccp_ctrl_has_cfgchg (struct sock *sk)
{
    struct my_sock 		*my_sk;
    int				ret;

    if (!sk) return 0;
    my_sk = mpdccp_my_sock (sk);
    if (!my_sk) return 0;	/* no mpdccp socket */
    rcu_read_lock();
    ret = (my_sk->link_cnt != mpdccp_link_cnt(my_sk->link_info));
    rcu_read_unlock();
    return ret;
}
EXPORT_SYMBOL (mpdccp_ctrl_has_cfgchg);

void
mpdccp_ctrl_cfgupdate (struct sock *sk)
{
    struct my_sock 		*my_sk;

    if (!sk) return;
    my_sk = mpdccp_my_sock (sk);
    if (!my_sk) return;	/* no mpdccp socket */
    rcu_read_lock();
    my_sk->link_cnt = mpdccp_link_cnt(my_sk->link_info);
    rcu_read_unlock();
}
EXPORT_SYMBOL (mpdccp_ctrl_cfgupdate);

#define LINK_UD_MAGIC	0x33a9c478
struct link_user_data {
	int			magic;
	void			*user_data;
	struct mpdccp_link_info	*link_info;
};

/* ****************************************************************************
 *  add / remove subflows - called by path manager
 * ****************************************************************************/

/* This function adds new sockets to existing connections:
 * - Attempts to establish connections to another endpoint as a client.
 */

int mpdccp_add_client_conn (	struct mpdccp_cb *mpcb,
				struct sockaddr *local_address,
				int locaddr_len,
				int if_idx,
				struct sockaddr *remote_address,
				int remaddr_len)
{
	int			ret = 0;
	int 			flags;
	struct socket   	*sock; /* The newly created socket */
	struct sock     	*sk;
	struct mpdccp_link_info	*link_info = NULL;
	
	if (!mpcb || !local_address || !remote_address) return -EINVAL;
	if (mpcb->role != MPDCCP_CLIENT) return -EINVAL;
	
	/* Create a new socket */
	ret = sock_create_kern(read_pnet(&mpcb->net), PF_INET, SOCK_DCCP, IPPROTO_DCCP, &sock);
	if (ret < 0) {
		mpdccp_pr_debug("Failed to create socket (%d).\n", ret);
		goto out;
	}
	
	sk = sock->sk;

	/* Get the service type from meta socket */
	dccp_sk(sk)->dccps_service = dccp_sk(mpcb->meta_sk)->dccps_service;

	set_mpdccp(sk, mpcb);
	
	ret = my_sock_init (sk, mpcb, if_idx, MPDCCP_CLIENT);
	if (ret < 0) {
		mpdccp_pr_debug("Failed to init mysock (%d).\n", ret);
		sock_release(sock);
		goto out;
	}
	
	/* Bind the socket to one of the DCCP-enabled IP addresses */
	ret = kernel_bind(sock, local_address, locaddr_len);
	if (ret < 0) {
		mpdccp_pr_debug("Failed to bind socket %p (%d).\n", sk, ret);
		sock_release(sock);
		goto out;
	}
	if (local_address->sa_family == AF_INET) {
		struct sockaddr_in 	*local_v4_address = (struct sockaddr_in*)local_address;
		link_info = mpdccp_link_find_ip4 (&init_net, &local_v4_address->sin_addr, NULL);
	} else if (local_address->sa_family == AF_INET6) {
		struct sockaddr_in6 	*local_v6_address = (struct sockaddr_in6*)local_address;
		link_info = mpdccp_link_find_ip6 (&init_net, &local_v6_address->sin6_addr, NULL);
	}
	if (!link_info) link_info = mpdccp_getfallbacklink (&init_net);
	
	mpdccp_my_sock(sk)->link_info = link_info;
	mpdccp_my_sock(sk)->link_cnt = mpdccp_link_cnt(link_info);
	mpdccp_my_sock(sk)->link_iscpy = 0;

	/* Add socket to the request list */
	spin_lock(&mpcb->psubflow_list_lock);
	list_add_tail_rcu(&mpdccp_my_sock(sk)->sk_list , &mpcb->prequest_list);
	mpdccp_pr_debug("Added new entry to prequest_list @ %p\n", mpdccp_my_sock(sk));
	spin_unlock(&mpcb->psubflow_list_lock);

	/* Only the first (key exchage) socket is blocking */
	flags = dccp_sk(sk)->is_kex_sk ? 0 : O_NONBLOCK;
	/* HACK: reduce retry timeout to 200ms for join sessions*/
	if (!dccp_sk(sk)->is_kex_sk) inet_csk(sk)->icsk_rto = ((unsigned int)HZ/5);
	ret = kernel_connect(sock, remote_address, remaddr_len, flags);
	if ((ret < 0) && (ret != -EINPROGRESS)) {
#ifdef CONFIG_IP_MPDCCP_DEBUG
		struct sockaddr_in *their_inaddr_ptr = (struct sockaddr_in *)remote_address; 
		mpdccp_pr_debug("Failed to connect from sk %pI4 %p (%d).\n",
			&their_inaddr_ptr->sin_addr, sk, ret);
#endif
		sock_release(sock);
		goto out;
	}

	if (dccp_sk(sk)->is_kex_sk && mpcb->kex_done) {
		u32 token;

		/* MP_KEY sockets can be authorized now. MP_JOIN sockets need to wait one more more ack */
		dccp_sk(sk)->auth_done = 1;

		/* Reset the flag to avoid inserting MP_KEY options in subsenquent ACKs */
		dccp_sk(sk)->is_kex_sk = 0;

		/* Key exchange is now completed, generate the path tokens */
		mpdccp_key_sha1(*(u64 *)mpcb->mpdccp_loc_key.value, *(u64 *)mpcb->mpdccp_rem_key.value, &token);
		mpcb->mpdccp_loc_token = token;
		mpdccp_key_sha1(*(u64 *)mpcb->mpdccp_rem_key.value, *(u64 *)mpcb->mpdccp_loc_key.value, &token);
		mpcb->mpdccp_rem_token = token;
		mpdccp_pr_debug("client: kex done lt: %x rt: %x", mpcb->mpdccp_loc_token, mpcb->mpdccp_rem_token);

		/* Update the state and MSS of meta socket */
		dccp_sk(mpcb->meta_sk)->dccps_mss_cache = dccp_sk(sk)->dccps_mss_cache;
		mpcb->meta_sk->sk_state = DCCP_OPEN;
	}

out:
	if (link_info && ret != 0) mpdccp_link_put (link_info);
	return ret;
}
EXPORT_SYMBOL (mpdccp_add_client_conn);

int mpdccp_add_listen_sock (	struct mpdccp_cb *mpcb,
				struct sockaddr *local_address,
				int locaddr_len,
				int if_idx)
{
    int                 retval	= 0;
    struct socket   	*sock; /* The newly created socket */
    struct sock     	*sk;
    struct mpdccp_link_info	*link_info = NULL;

    if (!mpcb || !local_address) return -EINVAL;
    if (mpcb->role != MPDCCP_SERVER) return -EINVAL;

    mpdccp_pr_debug ("Create subflow socket\n");
    /* Create a new socket */
    retval = sock_create(PF_INET, SOCK_DCCP, IPPROTO_DCCP, &sock);
    if (retval < 0) {
        mpdccp_pr_debug("Failed to create socket (%d).\n", retval);
        goto out;
    }
    sock->sk->sk_reuse = SK_FORCE_REUSE;

    sk = sock->sk;
    set_mpdccp(sk, mpcb);

    mpdccp_pr_debug ("init mysock\n");
    retval = my_sock_init (sk, mpcb, if_idx, MPDCCP_SERVER);
    if (retval < 0) {
        mpdccp_pr_debug("Failed to init mysock (%d).\n", retval);
        sock_release(sock);
        goto out;
    }

    mpdccp_pr_debug ("bind address: %pISc\n", local_address);
    /* Bind the socket to one of the DCCP-enabled IP addresses */
    retval = sock->ops->bind(sock, local_address, locaddr_len);
    if (retval < 0) {
        mpdccp_pr_debug("Failed to bind socket %p (%d).\n", sk, retval);
        sock_release(sock);
        goto out;
    }
    if (local_address->sa_family == AF_INET) {
         struct sockaddr_in 	*local_v4_address = (struct sockaddr_in*)local_address;
         link_info = mpdccp_link_find_ip4 (&init_net, &local_v4_address->sin_addr, NULL);
    } else if (local_address->sa_family == AF_INET6) {
         struct sockaddr_in6 	*local_v6_address = (struct sockaddr_in6*)local_address;
         link_info = mpdccp_link_find_ip6 (&init_net, &local_v6_address->sin6_addr, NULL);
    }
    if (!link_info) link_info = mpdccp_getfallbacklink (&init_net);

    mpdccp_my_sock(sk)->link_info = link_info;
    mpdccp_my_sock(sk)->link_cnt = mpdccp_link_cnt(link_info);
    mpdccp_my_sock(sk)->link_iscpy = 0;


    mpdccp_pr_debug ("set subflow to listen state\n");
    rcu_read_lock_bh();
    retval = sock->ops->listen(sock, MPDCCP_SERVER_BACKLOG);
    if (retval < 0) {
        mpdccp_pr_debug("Failed to listen on socket(%d).\n", retval);
        sock_release(sock);
        rcu_read_unlock_bh();
        goto out;
    }

    spin_lock(&mpcb->plisten_list_lock);
    list_add_tail_rcu(&mpdccp_my_sock(sk)->sk_list , &mpcb->plisten_list);
    mpcb->cnt_listensocks++;
    mpdccp_pr_debug("Added new entry to plisten_list @ %p\n", mpdccp_my_sock(sk));
    spin_unlock(&mpcb->plisten_list_lock);
    rcu_read_unlock_bh();

    mpdccp_pr_debug("server port added successfully. There are %d subflows now.\n",
			mpcb->cnt_subflows);

out:
    if (link_info && retval != 0) mpdccp_link_put (link_info);
    return retval;
}
EXPORT_SYMBOL (mpdccp_add_listen_sock);

int mpdccp_close_subflow (struct mpdccp_cb *mpcb, struct sock *sk, int destroy)
{
    if (!mpcb || !sk) return -EINVAL;
    mpdccp_pr_debug("enter for %p role %s state %d closing %d", sk, dccp_role(sk), sk->sk_state, mpdccp_my_sock(sk)->closing);

    /* This will call dccp_close() in process context (only once per socket) */
    if (!mpdccp_my_sock(sk)->closing) {
        mpdccp_my_sock(sk)->closing = 1;
        mpdccp_pr_debug("Close socket(%p)", sk);
        schedule_work(&mpdccp_my_sock(sk)->close_work);
    }
    return 0;
}
EXPORT_SYMBOL (mpdccp_close_subflow);

void mpdccp_handle_rem_addr(u32 del_path)
{
    struct sock *sk;
    struct mpdccp_cb *mpcb;
    mpdccp_pr_debug("enter handle_rem_addr");
        mpdccp_for_each_conn(pconnection_list, mpcb) {
            mpdccp_for_each_sk(mpcb, sk) {
                if(dccp_sk(sk)->id_rcv == del_path){
                mpdccp_close_subflow(mpcb, sk, 0);
                mpdccp_pr_debug("delete path %u sk %p", del_path, sk);
                }
            }
        }
}
EXPORT_SYMBOL (mpdccp_handle_rem_addr);

/*select sk to announce data*/

struct sock *mpdccp_select_ann_sock(struct mpdccp_cb *mpcb)
{

    struct sock *sk, *avsk = NULL;
    /*returns the first avilable socket - can be improved to 
     *the latest used or lowest rtt as in mptcp mptcp_select_ack_sock */

    mpdccp_for_each_sk(mpcb, sk) {

        if (!mpdccp_sk_can_send(sk))
            continue;
        avsk = sk;
        goto avfound;
    }

avfound:
    return avsk;
}
EXPORT_SYMBOL(mpdccp_select_ann_sock);

/*
 * the real xmit function
 */

int
mpdccp_xmit_to_sk (
	struct sock	*sk,
	struct sk_buff	*skb)
{
	int			len, ret=0;
	long 			timeo;
	struct mpdccp_cb	*mpcb;
	struct sock		*meta_sk;

	if (!skb || !sk) return -EINVAL;

	len = skb->len;
	if (len > dccp_sk(sk)->dccps_mss_cache) {
		dccp_sk(meta_sk)->dccps_mss_cache = dccp_sk(sk)->dccps_mss_cache;
		return -EMSGSIZE;
	}
	mpcb = get_mpcb (sk);
	meta_sk = mpcb ? mpcb->meta_sk : NULL;

	// TODO: check why was necessary
	//rcu_read_lock ();
	if(in_atomic())
		bh_lock_sock(sk);
	else
		lock_sock(sk);

	if (dccp_qpolicy_full(sk)) {
		ret = -EAGAIN;
		goto out_release;
	}

	timeo = sock_sndtimeo(sk, /* noblock */ 1);

	/*
	 * We have to use sk_stream_wait_connect here to set sk_write_pending,
	 * so that the trick in dccp_rcv_request_sent_state_process.
	 */
	/* Wait for a connection to finish. */
	if ((1 << sk->sk_state) & ~(DCCPF_OPEN | DCCPF_PARTOPEN))
		if ((ret = sk_stream_wait_connect(sk, &timeo)) != 0)
			goto out_release;

	if (skb->next && meta_sk) dccp_qpolicy_unlink (meta_sk, skb);
	skb_set_owner_w(skb, sk);
	dccp_qpolicy_push(sk, skb);

	if (!timer_pending(&dccp_sk(sk)->dccps_xmit_timer))
		dccp_write_xmit(sk);

	mpdccp_pr_debug("packet with %d bytes sent\n", len);

out_release:
	if(in_atomic())
		bh_unlock_sock(sk);
	else
		release_sock(sk);
	//rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(mpdccp_xmit_to_sk);

/* Process listen state by calling original backlog_rcv callback
 * and accept the connection */
int listen_backlog_rcv (struct sock *sk, struct sk_buff *skb)
{
    int ret = 0;    
    struct my_sock *my_sk = mpdccp_my_sock(sk);
    //struct mpdccp_cb *mpcb  = my_sk->mpcb;

    mpdccp_pr_debug("Executing backlog_rcv callback. sk %p my_sk %p bklog %p \n", sk, my_sk, my_sk->sk_backlog_rcv);

    if (my_sk->sk_backlog_rcv) {
	mpdccp_pr_debug("There is sk_backlog_rcv");
        ret = my_sk->sk_backlog_rcv (sk, skb);
    }
   
#if 0 
    /* If the queue was previously stopped because of a full cwnd,
    * a returning ACK will open the window again, so we should
    * re-enable the queue. */
    if (netif_queue_stopped(mpcb->mpdev->ndev) &&
        mpdccp_cwnd_available(mpcb)) {
        netif_wake_queue(mpcb->mpdev->ndev);
        MPSTATINC (mpcb->mpdev,tx_rearm);
    }
#endif
    
    /* We can safely ignore ret value of my_sk->sk_backlog_rcv, as it can only return 0 anyways
     * (and causes a LOT of pain if it was otherwise). */
    return ret;
}

void listen_data_ready (struct sock *sk)
{
    int ret;

    // Client side setup is not handled by this callback, use original workflow
    if (sk->sk_state == DCCP_REQUESTING) {
        struct my_sock *my_sk = mpdccp_my_sock(sk);
        if (my_sk->sk_data_ready)
            my_sk->sk_data_ready (sk);

        return;
    }

    /* If the socket is not listening, it does not belong to a server. */
    if (sk->sk_state == DCCP_LISTEN) {
        mpdccp_pr_debug("sk %p is in LISTEN state, not handled\n", sk);
         return;

    } 

    if ((sk->sk_state == DCCP_OPEN)
        || (sk->sk_state == DCCP_PARTOPEN)) {
        //mpdccp_pr_debug("sk %p is in OPEN state. Reading...\n", sk);

        ret = mpdccp_read_from_subflow (sk);
        if(ret < 0) {
            mpdccp_pr_debug("Failed to read message from sk %p (%d).\n", sk, ret);
        }
    }

    /* TODO: Work queues temporarily disabled. They led to a lot of 
     * packet loss. */
    // ret = queue_bnd_work(sk);
    // if(ret < 0)
    //     mpdccp_pr_debug("Failed to queue work (%d).\n", ret);

    return;
}

void mp_state_change(struct sock *sk)
{
    struct mpdccp_cb *mpcb;
    struct sock *subsk;
    mpdccp_pr_debug("enter sk %p role %s is_kex %d\n", sk, dccp_role(sk), dccp_sk(sk)->is_kex_sk);

    mpcb = MPDCCP_CB(sk);

    /* First (i.e. key exchange) subflow is ready at PARTOPEN state, the others need to wait to reach full OPEN due to extra ack */
    if ((sk->sk_state == DCCP_PARTOPEN && dccp_sk(sk)->is_kex_sk)
        || (sk->sk_state == DCCP_OPEN && !dccp_sk(sk)->is_kex_sk)) {

        /* Check if the subflow was already added */
        spin_lock(&mpcb->psubflow_list_lock);
        mpdccp_for_each_sk(mpcb, subsk) {
            if (sk == subsk) {
                mpdccp_pr_debug("sk %p already in subflow_list, skipping\n", sk);
                spin_unlock(&mpcb->psubflow_list_lock);
                goto out;
            }
        }

        /* Move socket from the request to the subflow list */
        list_move_tail(&mpdccp_my_sock(sk)->sk_list, &mpcb->psubflow_list);
        mpdccp_pr_debug("Added new entry sk %p to psubflow_list @ %p\n", sk, mpdccp_my_sock(sk));
        spin_unlock(&mpcb->psubflow_list_lock);

        mpcb->cnt_subflows = (mpcb->cnt_subflows) + 1;

        if (mpcb->sched_ops->init_subflow)
            mpcb->sched_ops->init_subflow(sk);

        mpdccp_report_new_subflow(sk);
        mpdccp_pr_debug("client connection established successfully. There are %d subflows now.\n", mpcb->cnt_subflows);
    }

out:
    return;
}

/* Based on mptcp_key_sha1@mptcp_ctrl.c */
/* modified to hash two keys */
void mpdccp_key_sha1(u64 key1, u64 key2, u32 *token)
{
    u32 workspace[SHA_WORKSPACE_WORDS];
    u32 mptcp_hashed_key[SHA_DIGEST_WORDS];
    u8 input[64];

    memset(workspace, 0, sizeof(workspace));

    /* Initialize input with appropriate padding */
    memset(&input[17], 0, sizeof(input) - 18); /* -18, because the last byte
                                               * is explicitly set too
                                               */
    memcpy(input, &key1, sizeof(key1)); /* Copy key1 to the msg beginning */
    memcpy(input+8, &key2, sizeof(key2)); /* Append key2 */
    input[16] = 0x80; /* Padding: First bit after message = 1 */
    input[63] = 0x80; /* Padding: Length of the message = 128 bits */

    sha_init(mptcp_hashed_key);
    sha_transform(mptcp_hashed_key, input, workspace);

    if (token)
        *token = mptcp_hashed_key[0];
}
EXPORT_SYMBOL(mpdccp_key_sha1);

/* General initialization of MPDCCP */
int mpdccp_ctrl_init(void)
{
    INIT_LIST_HEAD(&pconnection_list);
    spin_lock_init(&pconnection_list_lock);

    mpdccp_mysock_cache = kmem_cache_create("mpdccp_mysock", sizeof(struct my_sock),
                       0, SLAB_TYPESAFE_BY_RCU|SLAB_HWCACHE_ALIGN,
                       NULL);
    if (!mpdccp_mysock_cache) {
        mpdccp_pr_debug("Failed to create mysock slab cache.\n");
        return -ENOMEM;
    }

    mpdccp_cb_cache = kmem_cache_create("mpdccp_cb", sizeof(struct mpdccp_cb),
                       0, SLAB_TYPESAFE_BY_RCU|SLAB_HWCACHE_ALIGN,
                       NULL);
    if (!mpdccp_cb_cache) {
        mpdccp_pr_debug("Failed to create mpcb slab cache.\n");
    	kmem_cache_destroy(mpdccp_mysock_cache);
        return -ENOMEM;
    }

    /*
     * The number of active work items is limited by the number of
     * connections, so leave @max_active at default.
     */
    mpdccp_wq = alloc_workqueue("mpdccp_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!mpdccp_wq) {
        mpdccp_pr_debug("Failed to register sysctl.\n");
    	kmem_cache_destroy(mpdccp_mysock_cache);
    	kmem_cache_destroy(mpdccp_cb_cache);
	return -1;
    }
    return 0;
}

void mpdccp_ctrl_finish(void)
{
    if (mpdccp_wq) {
        mpdccp_wq_flush();
        destroy_workqueue(mpdccp_wq);
        mpdccp_wq = NULL;
    }

    kmem_cache_destroy(mpdccp_mysock_cache);
    kmem_cache_destroy(mpdccp_cb_cache);

#if 0
    /* sk_free (and __sk_free) requires wmem_alloc to be 1.
     * All the rest is set to 0 thanks to __GFP_ZERO above.
     */
    atomic_set(&master_sk->sk_wmem_alloc, 1);
    sk_free(master_sk);
#endif
}
