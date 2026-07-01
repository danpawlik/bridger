// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Felix Fietkau <nbd@nbd.name>
 */
#include <string.h>
#include <stdlib.h>
#include "bridger.h"

static int fdb_key_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, sizeof(struct fdb_key));
}

void fdb_init(struct bridge *br)
{
	avl_init(&br->fdb, fdb_key_cmp, false, NULL);
}

struct fdb_entry *fdb_get(struct bridge *br, const struct fdb_key *key)
{
	struct fdb_entry *f;

	return avl_find_element(&br->fdb, key, f, node);
}

struct fdb_entry *fdb_create(struct bridge *br, const struct fdb_key *key, struct device *dev)
{
	struct fdb_entry *f;

	f = fdb_get(br, key);
	if (f) {
		fdb_set_device(f, dev);
		return f;
	}

	D("Create fdb vlan %d entry %s on %s\n",
	  key->vlan, format_macaddr(key->addr), dev ? dev->ifname : "(none)");

	f = calloc(1, sizeof(*f));
	memcpy(&f->key, key, sizeof(*key));
	f->node.key = &f->key;
	INIT_LIST_HEAD(&f->dev_list);
	INIT_LIST_HEAD(&f->flows_in);
	INIT_LIST_HEAD(&f->flows_out);
	avl_insert(&br->fdb, &f->node);
	fdb_set_device(f, dev);

	return f;
}

void fdb_delete(struct bridge *br, struct fdb_entry *f)
{
	D("Delete fdb vlan %d entry %s\n", f->key.vlan, format_macaddr(f->key.addr));
	fdb_set_device(f, NULL);
	avl_delete(&br->fdb, &f->node);
	free(f);
}

void fdb_set_device(struct fdb_entry *f, struct device *dev)
{
	struct device *old_dev;

	if (f->dev == dev)
		return;

	old_dev = f->dev;

	if (f->dev)
		D("Set fdb vlan %d entry %s device to %s\n",
		  f->key.vlan, format_macaddr(f->key.addr), dev ? dev->ifname : "(none)");

	fdb_clear_flows(f);
	if (!list_empty(&f->dev_list))
		list_del_init(&f->dev_list);
	f->dev = dev;
	if (dev)
		list_add(&f->dev_list, &dev->fdb_entries);

	/*
	 * On roam (old_dev → new dev, not disconnect): purge any pending_flows
	 * BPF map entries that arrived on old_dev.  Without this, the next poll
	 * cycle processes those stale entries, detects a backwards port migration
	 * (fdb_in->dev now points to new_dev, but the stale key carries
	 * old_dev's ifindex), and calls fdb_set_device() again in the wrong
	 * direction — re-creating TC offload filters for the departed radio and
	 * undoing the cleanup that fdb_clear_flows() just performed.
	 */
	if (old_dev && !old_dev->br && dev)
		bridger_bpf_flush_pending_by_dev(old_dev);
}

void fdb_clear_flows(struct fdb_entry *f)
{
	struct bridger_flow *flow, *tmp;

	list_for_each_entry_safe(flow, tmp, &f->flows_in, fdb_in_list)
		bridger_flow_delete(flow);
	list_for_each_entry_safe(flow, tmp, &f->flows_out, fdb_out_list)
		bridger_flow_delete(flow);
}
