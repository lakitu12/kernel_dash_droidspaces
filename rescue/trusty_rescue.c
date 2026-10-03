/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trusty_rescue — self-guarded camera-chain rescue for gz-erased (nVHE/pKVM) boots.
 *
 * Background (all field-verified on dash / MT6991Z, GKI android15-6.6):
 *   When the GenieZone/Trusty (gz) firmware is erased to give the Linux kernel EL2
 *   (kvm-arm.mode=nvhe, for pKVM/DroidSpaces VMs), the AP-side trusty core fails
 *   its SMC version handshake ("GZ SMC version(0) is not supported"), so
 *   gz_trusty_mod's trusty_probe() returns -22 and never runs its trailing
 *   of_platform_populate("/trusty"). The /trusty/trusty-virtio device is therefore
 *   never created. That device is the supplier of gce-mbox-sec / gce-mbox-m-sec
 *   (secure cmdq mailbox), which is a supplier of camera-imgsys-cmdq -> imgsys-fw.
 *   The whole chain defers forever: IMGSCP never powers up, the ISP media topology
 *   stays empty (camisp media0 has 0 entities), and camerahalserver SIGABRTs on
 *   "findMediaEntityDevnodePath mMediaTopologyList.size=0 != 1".
 *
 * Fix, reproduced in-kernel from the proven trusty-pop.kpm recipe (field-verified
 * 2026-10-03 on this very device: all three cameras + stills + video restored
 * under nvhe / gz-erased; see pstore + media_enum + DCIM evidence trail):
 *   1) self-guarded, three-legged fingerprint, must hold continuously for
 *      GRACE_SECS before any action:
 *        a. trusty-virtio DRIVER registered  (vendor_dlkm gz stack loaded)
 *        b. trusty-virtio DEVICE absent      (trusty_probe never populated it)
 *        c. /trusty PARENT device UNBOUND    (trusty_probe itself failed)
 *      On a healthy gz boot the parent binds + children populate within ms of
 *      module load, so leg (c) alone excludes the healthy case structurally;
 *      worst case on anything unusual we simply never fire.
 *   2) unregister gz_virtio_mod's trusty_virtio_driver BEFORE creating the device
 *      (its probe dereferences a NULL trustye and ipanics on bind — the exact
 *      v1-kpm crash, pstore-verified), pinning the owner module first. The pin
 *      is permanent and intentional: the module can then never rmmod, so its
 *      exit path double-unregistering our struct can never run;
 *   3) create /trusty/trusty-virtio as a bare "exists" marker device: secure-cmdq
 *      only needs the supplier to EXIST (probe does of_find_compatible_node +
 *      device_link_add, no SMC — disasm-verified); no driver bound => no probe;
 *   4) driver_deferred_probe_trigger() unrolls the chain without userspace.
 *
 * Driver-lookup API note (CI run #1 lesson): driver_find()'s device ref pairs
 * with the PRIVATE put_driver(), un-expressible from public headers. We iterate
 * with bus_for_each_drv() (callback receives the raw driver pointer, takes no
 * device ref) and stabilize it by pinning the owner module inside the callback.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/err.h>

/* defined in drivers/base/dd.c, not declared in public headers */
void driver_deferred_probe_trigger(void);

/*
 * Bus-registered driver name, field-verified. Note there is a SIBLING driver
 * "ise-trusty-virtio" (ise stack, separate sysfs dir) — strcmp exact-match
 * never hits it. The gz-side name was confirmed by the KPM v2 identity check
 * (driver.name == "trusty-virtio" + probe ptr == trusty_virtio_probe).
 */
#define VIRTIO_DRV_NAME   "trusty-virtio"
#define PARENT_NODE_NAME  "trusty"
#define VIRTIO_NODE_NAME  "trusty-virtio"
#define GRACE_SECS        15   /* full fingerprint must hold this long before we act */

static int settle_secs = 90;
module_param(settle_secs, int, 0644);
MODULE_PARM_DESC(settle_secs, "max seconds to wait for the gz-erased fingerprint to stabilize");

static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "enable gz-erased trusty-virtio rescue (default on, self-guarded)");

static bool rescued;   /* one-shot latch */

/* ---------- DT helpers (of_find_node_by_name/of_get_child_by_name are
 * EXPORT_SYMBOL-confirmed; of_find_node_by_path hidden from runtime kallsyms
 * by the KPM toolchain's spoof layer, child-walk sidesteps the question) ---------- */

static struct device_node *trusty_parent_np(void)
{
	return of_find_node_by_name(NULL, PARENT_NODE_NAME);
}

static struct device_node *trusty_virtio_np(struct device_node *parent)
{
	return of_get_child_by_name(parent, VIRTIO_NODE_NAME);
}

/*
 * Look up the trusty-virtio platform device by walking the DT:
 *   root -> node named "trusty" -> child "trusty-virtio".
 * ERR_PTR(-ENODEV) = DT shape absent -> foreign platform / stripped DTB: no-op forever
 * NULL             = node exists, device absent (the gz-erased case)
 * otherwise        = pdev (ref held) -> healthy gz, or we already rescued
 */
static struct platform_device *lookup_virtio_device(void)
{
	struct device_node *parent, *np;
	struct platform_device *pdev;

	parent = trusty_parent_np();
	if (!parent)
		return ERR_PTR(-ENODEV);
	np = trusty_virtio_np(parent);
	of_node_put(parent);
	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	return pdev;
}

/*
 * Leg (c): is the /trusty parent platform device bound to its driver?
 * Healthy gz: trusty_probe binds within ms of module load.
 * gz-erased: trusty_probe failed -22, parent forever unbound.
 * Parent pdev not created yet also counts as unbound (modules still coming up;
 * legs (a)+(b) and the grace window cover that phase).
 */
static bool trusty_parent_bound(void)
{
	struct device_node *parent = trusty_parent_np();
	struct platform_device *pdev;
	bool bound;

	if (!parent)
		return true;   /* no DT shape: stay out */
	pdev = of_find_device_by_node(parent);
	of_node_put(parent);
	if (!pdev)
		return false;
	bound = pdev->dev.driver != NULL;
	platform_device_put(pdev);
	return bound;
}

/* ---------- driver lookup, public APIs only ---------- */

struct drv_ctx {
	const char *name;
	struct device_driver *drv;
	bool pin;
	bool pinned;
};

static int drv_cb(struct device_driver *d, void *data)
{
	struct drv_ctx *c = data;

	if (strcmp(d->name, c->name))
		return 0;
	if (c->pin) {
		/* pin owner *inside* iteration (same lock context as the list walk,
		 * no race window). If pinning fails the module is going away:
		 * do NOT expose the pointer, stop with no result. */
		if (d->owner && !try_module_get(d->owner))
			return 1;          /* stop scanning, c->drv stays NULL */
		c->pinned = true;
	}
	c->drv = d;
	return 1;                          /* stop iteration */
}

/*
 * Find a platform driver by name. When pin=true, the owner module is refcounted
 * (permanently for our use) so the returned pointer's memory stays valid —
 * and, by construction, the module can never rmmod behind our back.
 */
static struct device_driver *drv_lookup(const char *name, bool pin)
{
	struct drv_ctx c = { .name = name, .drv = NULL, .pin = pin, .pinned = false };

	bus_for_each_drv(&platform_bus_type, NULL, &c, drv_cb);
	return c.drv;
}

/* ---------- rescue ---------- */

/* 0 done, -EAGAIN retry later, <0 skip. */
static int do_rescue(void)
{
	struct device_node *parent, *np;
	struct platform_device *parent_pdev, *new_pdev, *existing;
	struct device_driver *drv;
	int ret;

	/* Re-confirm the full fingerprint immediately before touching anything. */
	if (trusty_parent_bound()) {
		pr_info("parent bound - no-op\n");
		return 0;
	}
	existing = lookup_virtio_device();
	if (IS_ERR(existing))
		return -ENODEV;
	if (existing) {
		platform_device_put(existing);
		pr_info("device appeared before rescue - no-op\n");
		return 0;
	}

	parent = trusty_parent_np();
	if (!parent)
		return -ENODEV;
	np = trusty_virtio_np(parent);
	if (!np) {
		of_node_put(parent);
		return -ENODEV;
	}
	parent_pdev = of_find_device_by_node(parent);
	of_node_put(parent);
	if (!parent_pdev) {
		pr_info("/trusty pdev not ready, retry later\n");
		of_node_put(np);
		return -EAGAIN;
	}

	/* 2) Unregister the crash-prone driver (owner pinned atomically inside). */
	drv = drv_lookup(VIRTIO_DRV_NAME, true);
	if (!drv) {
		pr_info("driver vanished or owner going away, retry later\n");
		ret = -EAGAIN;
		goto out;
	}
	driver_unregister(drv);
	pr_info("unregistered trusty_virtio_driver (owner pinned against removal)\n");

	/* 3) Bare marker device: driver already gone => device_add finds no match
	 *    => no probe => no NULL-trustye deref. */
	new_pdev = of_platform_device_create(np, NULL, &parent_pdev->dev);
	if (!new_pdev) {
		pr_err("of_platform_device_create failed\n");
		ret = -ENOMEM;
		goto out;
	}
	pr_info("created trusty-virtio marker device\n");

	/* 4) Kick deferred queue: gce-mbox-sec supplier now exists. */
	driver_deferred_probe_trigger();
	pr_info("deferred probes triggered; camera chain should unroll\n");
	ret = 0;

out:
	of_node_put(np);
	platform_device_put(parent_pdev);
	return ret;
}

static void rescue_work(struct work_struct *w)
{
	int i, seen_at = -1;

	if (!enable) {
		pr_info("disabled by param, no-op\n");
		return;
	}

	for (i = 0; i < settle_secs; i++) {
		struct platform_device *dev = lookup_virtio_device();

		if (IS_ERR(dev)) {
			pr_info("no trusty DT node, staying out forever\n");
			return;
		}
		if (dev) {
			platform_device_put(dev);
			/* After our rescue the device exists and the parent stays
			 * unbound by design — that is the expected terminal state,
			 * NOT "gz healthy". Before any rescue it means gz is fine. */
			if (rescued)
				pr_info("trusty-virtio device present at +%ds (post-rescue, expected) - done\n", i);
			else if (trusty_parent_bound())
				pr_info("trusty-virtio present + parent bound at +%ds (gz healthy) - no-op\n", i);
			else {
				/* device there but parent unbound: foreign state we
				 * didn't cause; keep hands off, keep watching */
				pr_info("device present, parent unbound at +%ds - unexpected, no-op\n", i);
				return;
			}
			return;
		}

		/* three-legged fingerprint: (a) driver up (b) device absent (c) parent unbound */
		if (drv_lookup(VIRTIO_DRV_NAME, false) && !trusty_parent_bound()) {
			if (seen_at < 0)
				seen_at = i;
		} else {
			seen_at = -1;
		}

		if (seen_at >= 0 && (i - seen_at) >= GRACE_SECS) {
			pr_info("gz-erased fingerprint stable %ds (driver up, device absent, parent unbound) - rescuing\n",
				i - seen_at);
			if (do_rescue() != -EAGAIN) {
				rescued = true;
				return;
			}
			seen_at = -1;
		}
		msleep(1000);
	}

	pr_warn("gave up after %ds (fingerprint never stable) - stayed safe\n", settle_secs);
}
static DECLARE_DELAYED_WORK(trusty_rescue_work, rescue_work);

static int __init trusty_rescue_init(void)
{
	/* Start after first-stage init hands off to vendor_dlkm module loading;
	 * the work item keeps polling anyway, so the exact delay isn't critical. */
	schedule_delayed_work(&trusty_rescue_work, msecs_to_jiffies(3000));
	pr_info("loaded (settle_secs=%d, enable=%d)\n", settle_secs, enable);
	return 0;
}

static void __exit trusty_rescue_exit(void)
{
	cancel_delayed_work_sync(&trusty_rescue_work);
}

late_initcall(trusty_rescue_init);
module_exit(trusty_rescue_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Self-guarded trusty-virtio/camera-chain rescue for gz-erased boots");
