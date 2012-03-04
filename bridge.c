#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "netifd.h"
#include "device.h"
#include "interface.h"
#include "system.h"

enum {
	BRIDGE_ATTR_IFNAME,
	BRIDGE_ATTR_STP,
	BRIDGE_ATTR_FORWARD_DELAY,
	BRIDGE_ATTR_IGMP_SNOOP,
	BRIDGE_ATTR_AGEING_TIME,
	BRIDGE_ATTR_HELLO_TIME,
	BRIDGE_ATTR_MAX_AGE,
	__BRIDGE_ATTR_MAX
};

static const struct blobmsg_policy bridge_attrs[__BRIDGE_ATTR_MAX] = {
	[BRIDGE_ATTR_IFNAME] = { "ifname", BLOBMSG_TYPE_ARRAY },
	[BRIDGE_ATTR_STP] = { "stp", BLOBMSG_TYPE_BOOL },
	[BRIDGE_ATTR_FORWARD_DELAY] = { "forward_delay", BLOBMSG_TYPE_INT32 },
	[BRIDGE_ATTR_AGEING_TIME] = { "ageing_time", BLOBMSG_TYPE_INT32 },
	[BRIDGE_ATTR_HELLO_TIME] = { "hello_time", BLOBMSG_TYPE_INT32 },
	[BRIDGE_ATTR_MAX_AGE] = { "max_age", BLOBMSG_TYPE_INT32 },
	[BRIDGE_ATTR_IGMP_SNOOP] = { "igmp_snooping", BLOBMSG_TYPE_BOOL },
};

static const union config_param_info bridge_attr_info[__BRIDGE_ATTR_MAX] = {
	[BRIDGE_ATTR_IFNAME] = { .type = BLOBMSG_TYPE_STRING },
};

static const struct config_param_list bridge_attr_list = {
	.n_params = __BRIDGE_ATTR_MAX,
	.params = bridge_attrs,
	.info = bridge_attr_info,

	.n_next = 1,
	.next = { &device_attr_list },
};

static struct device *bridge_create(const char *name, struct blob_attr *attr);
static void bridge_config_init(struct device *dev);
static void bridge_free(struct device *dev);
static void bridge_dump_info(struct device *dev, struct blob_buf *b);

const struct device_type bridge_device_type = {
	.name = "Bridge",
	.config_params = &bridge_attr_list,

	.create = bridge_create,
	.config_init = bridge_config_init,
	.free = bridge_free,
	.dump_info = bridge_dump_info,
};

struct bridge_state {
	struct device dev;
	device_state_cb set_state;

	struct bridge_config config;
	struct blob_attr *ifnames;
	bool active;
	bool force_active;

	struct vlist_tree members;
	int n_present;
};

struct bridge_member {
	struct vlist_node node;
	struct bridge_state *bst;
	struct device_user dev;
	bool present;
};

static int
bridge_disable_member(struct bridge_member *bm)
{
	struct bridge_state *bst = bm->bst;

	if (!bm->present)
		return 0;

	system_bridge_delif(&bst->dev, bm->dev.dev);
	device_release(&bm->dev);

	return 0;
}

static int
bridge_enable_member(struct bridge_member *bm)
{
	struct bridge_state *bst = bm->bst;
	int ret;

	if (!bm->present)
		return 0;

	ret = device_claim(&bm->dev);
	if (ret < 0)
		goto error;

	ret = system_bridge_addif(&bst->dev, bm->dev.dev);
	if (ret < 0)
		goto error;

	return 0;

error:
	bm->present = false;
	bst->n_present--;
	return ret;
}

static void
bridge_remove_member(struct bridge_member *bm)
{
	struct bridge_state *bst = bm->bst;

	if (!bm->present)
		return;

	bm->present = false;
	bm->bst->n_present--;
	if (bst->dev.active)
		bridge_disable_member(bm);

	bst->force_active = false;
	if (bst->n_present == 0)
		device_set_present(&bst->dev, false);
}

static void
bridge_member_cb(struct device_user *dev, enum device_event ev)
{
	struct bridge_member *bm = container_of(dev, struct bridge_member, dev);
	struct bridge_state *bst = bm->bst;

	switch (ev) {
	case DEV_EVENT_ADD:
		assert(!bm->present);

		bm->present = true;
		bst->n_present++;

		if (bst->dev.active)
			bridge_enable_member(bm);
		else if (bst->n_present == 1)
			device_set_present(&bst->dev, true);

		break;
	case DEV_EVENT_REMOVE:
		if (!bm->present)
			return;

		if (dev->hotplug)
			vlist_delete(&bst->members, &bm->node);
		else
			bridge_remove_member(bm);

		break;
	default:
		return;
	}
}

static int
bridge_set_down(struct bridge_state *bst)
{
	struct bridge_member *bm;

	bst->set_state(&bst->dev, false);

	vlist_for_each_element(&bst->members, bm, node)
		bridge_disable_member(bm);

	system_bridge_delbr(&bst->dev);

	return 0;
}

static int
bridge_set_up(struct bridge_state *bst)
{
	struct bridge_member *bm;
	int ret;

	if (!bst->force_active && !bst->n_present)
		return -ENOENT;

	ret = system_bridge_addbr(&bst->dev, &bst->config);
	if (ret < 0)
		goto out;

	vlist_for_each_element(&bst->members, bm, node)
		bridge_enable_member(bm);

	if (!bst->force_active && !bst->n_present) {
		/* initialization of all member interfaces failed */
		system_bridge_delbr(&bst->dev);
		device_set_present(&bst->dev, false);
		return -ENOENT;
	}

	ret = bst->set_state(&bst->dev, true);
	if (ret < 0)
		bridge_set_down(bst);

out:
	return ret;
}

static int
bridge_set_state(struct device *dev, bool up)
{
	struct bridge_state *bst;

	bst = container_of(dev, struct bridge_state, dev);

	if (up)
		return bridge_set_up(bst);
	else
		return bridge_set_down(bst);
}

static struct bridge_member *
bridge_create_member(struct bridge_state *bst, struct device *dev, bool hotplug)
{
	struct bridge_member *bm;
	char *name;

	bm = calloc(1, sizeof(*bm) + strlen(dev->ifname) + 1);
	bm->bst = bst;
	bm->dev.cb = bridge_member_cb;
	bm->dev.hotplug = hotplug;
	name = (char *) (bm + 1);
	strcpy(name, dev->ifname);
	vlist_add(&bst->members, &bm->node, name);

	device_add_user(&bm->dev, dev);

	return bm;
}

static void
bridge_member_update(struct vlist_tree *tree, struct vlist_node *node_new,
		     struct vlist_node *node_old)
{
	struct bridge_member *bm;

	if (node_new && node_old)
		return;

	if (node_old) {
		bm = container_of(node_old, struct bridge_member, node);
		bridge_remove_member(bm);
		device_remove_user(&bm->dev);
		free(bm);
	}
}


static void
bridge_add_member(struct bridge_state *bst, const char *name)
{
	struct device *dev;

	dev = device_get(name, true);
	if (!dev)
		return;

	bridge_create_member(bst, dev, false);
}

static int
bridge_hotplug_add(struct device *dev, struct device *member)
{
	struct bridge_state *bst = container_of(dev, struct bridge_state, dev);

	bridge_create_member(bst, member, true);

	return 0;
}

static int
bridge_hotplug_del(struct device *dev, struct device *member)
{
	struct bridge_state *bst = container_of(dev, struct bridge_state, dev);
	struct bridge_member *bm;

	bm = vlist_find(&bst->members, member->ifname, bm, node);
	if (!bm)
		return UBUS_STATUS_NOT_FOUND;

	vlist_delete(&bst->members, &bm->node);
	return 0;
}

static int
bridge_hotplug_prepare(struct device *dev)
{
	struct bridge_state *bst;

	bst = container_of(dev, struct bridge_state, dev);
	bst->force_active = true;
	device_set_present(&bst->dev, true);

	return 0;
}

static const struct device_hotplug_ops bridge_ops = {
	.prepare = bridge_hotplug_prepare,
	.add = bridge_hotplug_add,
	.del = bridge_hotplug_del
};

static void
bridge_free(struct device *dev)
{
	struct bridge_state *bst;

	device_cleanup(dev);
	bst = container_of(dev, struct bridge_state, dev);
	vlist_flush_all(&bst->members);
	free(bst);
}

static void
bridge_dump_info(struct device *dev, struct blob_buf *b)
{
	struct bridge_state *bst;
	struct bridge_member *bm;
	void *list;

	bst = container_of(dev, struct bridge_state, dev);

	system_if_dump_info(dev, b);
	list = blobmsg_open_array(b, "bridge-members");

	vlist_for_each_element(&bst->members, bm, node)
		blobmsg_add_string(b, NULL, bm->dev.dev->ifname);

	blobmsg_close_array(b, list);
}

static void
bridge_config_init(struct device *dev)
{
	struct bridge_state *bst;
	struct blob_attr *cur;
	int rem;

	bst = container_of(dev, struct bridge_state, dev);

	if (!bst->ifnames)
		return;

	blobmsg_for_each_attr(cur, bst->ifnames, rem) {
		bridge_add_member(bst, blobmsg_data(cur));
	}
}

static void
bridge_apply_settings(struct bridge_state *bst, struct blob_attr **tb)
{
	struct bridge_config *cfg = &bst->config;
	struct blob_attr *cur;

	/* defaults */
	cfg->stp = true;
	cfg->forward_delay = 1;
	cfg->igmp_snoop = true;

	if ((cur = tb[BRIDGE_ATTR_STP]))
		cfg->stp = blobmsg_get_bool(cur);

	if ((cur = tb[BRIDGE_ATTR_FORWARD_DELAY]))
		cfg->forward_delay = blobmsg_get_u32(cur);

	if ((cur = tb[BRIDGE_ATTR_IGMP_SNOOP]))
		cfg->igmp_snoop = blobmsg_get_bool(cur);

	if ((cur = tb[BRIDGE_ATTR_AGEING_TIME])) {
		cfg->ageing_time = blobmsg_get_u32(cur);
		cfg->flags |= BRIDGE_OPT_AGEING_TIME;
	}

	if ((cur = tb[BRIDGE_ATTR_HELLO_TIME])) {
		cfg->hello_time = blobmsg_get_u32(cur);
		cfg->flags |= BRIDGE_OPT_HELLO_TIME;
	}

	if ((cur = tb[BRIDGE_ATTR_MAX_AGE])) {
		cfg->max_age = blobmsg_get_u32(cur);
		cfg->flags |= BRIDGE_OPT_MAX_AGE;
	}
}

static struct device *
bridge_create(const char *name, struct blob_attr *attr)
{
	struct blob_attr *tb_dev[__DEV_ATTR_MAX];
	struct blob_attr *tb_br[__BRIDGE_ATTR_MAX];
	struct bridge_state *bst;
	struct device *dev = NULL;

	blobmsg_parse(device_attr_list.params, __DEV_ATTR_MAX, tb_dev,
		blob_data(attr), blob_len(attr));
	blobmsg_parse(bridge_attrs, __BRIDGE_ATTR_MAX, tb_br,
		blob_data(attr), blob_len(attr));

	bst = calloc(1, sizeof(*bst));
	if (!bst)
		return NULL;

	dev = &bst->dev;
	device_init(dev, &bridge_device_type, name);
	device_init_settings(dev, tb_dev);
	dev->config_pending = true;
	bst->ifnames = tb_br[BRIDGE_ATTR_IFNAME];
	bridge_apply_settings(bst, tb_br);

	bst->set_state = dev->set_state;
	dev->set_state = bridge_set_state;

	dev->hotplug_ops = &bridge_ops;

	vlist_init(&bst->members, avl_strcmp, bridge_member_update);

	return dev;
}


