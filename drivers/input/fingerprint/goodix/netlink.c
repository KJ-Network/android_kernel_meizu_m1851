/*
 * netlink interface
 *
 * Copyright (c) 2017 Goodix
 * Copyright (C) 2018 XiaoMi, Inc.
 * Copyright (C) 2026 Kopsources.ORG
 */
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>

#include "gf_spi.h"

static struct sock *gf_nl_sock;
static u32 gf_nl_pid;

void sendnlmsg(u8 event)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;

	if (!gf_nl_sock || !gf_nl_pid)
		return;
	skb = nlmsg_new(GF_NET_MAX, GFP_ATOMIC);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, GF_NET_MAX, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	memset(nlmsg_data(nlh), 0, GF_NET_MAX);
	*((u8 *)nlmsg_data(nlh)) = event;
	NETLINK_CB(skb).portid = 0;
	(void)netlink_unicast(gf_nl_sock, skb, gf_nl_pid, MSG_DONTWAIT);
}

static void nl_data_ready(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;

	if (!skb || skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	gf_nl_pid = nlh->nlmsg_pid;
}

int netlink_init(void)
{
	struct netlink_kernel_cfg cfg = { .input = nl_data_ready };

	gf_nl_sock = netlink_kernel_create(&init_net, GF_NETLINK, &cfg);
	return gf_nl_sock ? 0 : -ENOMEM;
}

void netlink_exit(void)
{
	if (gf_nl_sock) {
		netlink_kernel_release(gf_nl_sock);
		gf_nl_sock = NULL;
	}
	gf_nl_pid = 0;
}
