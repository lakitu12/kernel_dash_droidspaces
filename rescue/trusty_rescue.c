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
 * 2026-10-03: all three cameras + stills + video restored under nvhe/gz-erased):
 *   1) self-guarded fingerprint: the trusty-virtio DRIVER is registered (vendor_dlkm
 *      loaded) but its DEVICE stays absent across GRACE_SECS => gz-erased. On a
 *      healthy gz boot, trusty_probe creates the device within ms of the driver
 *      registering, so the device is always present by then and we no-op;
 *   2) unregister gz_virtio_mod's trusty_virtio_driver BEFORE creating the device
 *      (its probe dereferences a NULL trustye and would ipanic on bind — the exact
 *      v1-kpm crash, pstore-verified), and permanently pin the owning module so
 *      its exit path can never double-unregister the embedded struct (UAF-safe);
 *   3) create /trusty/trusty-virtio as a bare "exists" marker device: secure-cmdq
 *      only needs the supplier to exist (of_find_compatible_node + device_link_add,
 *      no SMC in probe — disasm-verified), no driver bound, no probe, no deref;
 *   4) driver_deferred_probe_trigger() so the chain resolves without userspace.
 *
 * DT shape (from dash vendor_boot DTB): /trusty { ... trusty-virtio { ... } }
 * Uses of_find_node_by_name/of_get_child_by_name (both EXPORT_SYMBOL-verified)
 * instead of of_find_node_by_path (present in vmlinux but hidden from runtime
 * kallsyms by the KPM toolchain's spoof layer — child-walk avoids the question).
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
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/err.h>

/* exported (EXPORT_SYMBOL_GPL, drivers/base/bus.c) but not declared in public
 * headers on 6.6 */
struct device_driver *driver_find(const char *name, const struct bus_type *bus);
/* defined in drivers/base/dd.c, not in a public header */
void driver_deferred_probe_trigger(void);

#define VIRTIO_DRV_NAME   "trusty-virtio"   /* == struct device_driver.name (DTB .rodata verified) */
#define PARENT_NODE_NAME  "trusty"
#define VIRTIO_NODE_NAME  "trusty-virtio"
#define GRACE_SECS        5

static int settle_secs = 60;
module_param(settle_secs, int, 0644);
MODULE_PARM_DESC(settle_secs, "max seconds to wait for a healthy trusty_probe to create the device");

static bool enable = true;
module_param(enable, bool, 0644);
MODULE_PARM_DESC(enable, "enable gz-erased trusty-virtio rescue (default on, self-guarded)");

static bool rescued;   /* one-shot latch */

/*
 * Look up the trusty-virtio platform device by walking
 * root -> node named "trusty" -> child "trusty-virtio".
 *  ERR_PTR(-ENODEV): DT shape absent -> foreign platform / no trusty -> never rescue.
 *  NULL:             node exists, device absent -> gz-erased fingerprint.
 *  else:             pdev (ref held) -> healthy (gz intact) or already rescued.
 */
static struct platform_device *lookup_virtio_device(void)
{
	struct device_node *parent, *np;
	struct platform_device *pdev;

	parent = of_find_node_by_name(NULL, PARENT_NODE_NAME);
	if (!parent)
		return ERR_PTR(-ENODEV);
	np = of_get_child_by_name(parent, VIRTIO_NODE_NAME);
	of_node_put(parent);
	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	return pdev;
}

/* 0 done, -EAGAIN retry later, <0 skip. */
static int do_rescue(void)
{
	struct device_node *parent, *np;
	struct platform_device *parent_pdev, *new_pdev, *existing;
	struct device_driver *drv;
	int ret;

	/* Re-confirm absence immediately before touching anything. */
	existing = lookup_virtio_device();
	if (IS_ERR(existing))
		return -ENODEV;
	if (existing) {
		platform_device_put(existing);
		pr_info("device appeared before rescue - no-op\n");
		return 0;
	}

	/* Resolve parent pdev + virtio np. */
	parent = of_find_node_by_name(NULL, PARENT_NODE_NAME);
	if (!parent)
		return -ENODEV;
	np = of_get_child_by_name(parent, VIRTIO_NODE_NAME);
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

	/*
	 * 2) Pin the owner first (refcnt +1 forever): with a live reference
	 *    gz_virtio_mod can never rmmod, so its module-exit
	 *    __platform_driver_unregister() on our already-unregistered driver
	 *    can never run -> no list-UAF. This pin is intentional; on a
	 *    gz-erased boot the driver is non-functional anyway.
	 */
	drv = driver_find(VIRTIO_DRV_NAME, &platform_bus_type);
	if (!drv) {
		pr_err("trusty_virtio_driver vanished unexpectedly\n");
		ret = -ENODEV;
		goto out;
	}
	if (drv->owner && !try_module_get(drv->owner)) {
		pr_err("owner module going away, skip\n");
		put_device(&drv->p);   /* driver_find() took a ref; drop it */
		ret = -EBUSY;
		goto out;
	}
	driver_unregister(drv);
	put_device(&drv->p);   /* balance driver_find()'s ref */
	pr_info("unregistered trusty_virtio_driver (owner pinned against removal)\n");

	/* 3) Bare marker device. Driver already gone => device_add finds no match
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
	/* np ref owned from entry to here on every out: path */
	of_node_put(np);
	platform_device_put(parent_pdev);
	return ret;
}

static void rescue_work(struct work_struct *w)
{
	int i, drv_seen_at = -1;

	if (!enable) {
		pr_info("disabled by param, no-op\n");
		return;
	}

	for (i = 0; i < settle_secs; i++) {
		struct platform_device *dev = lookup_virtio_device();

		if (IS_ERR(dev)) {
			/* No /trusty/trusty-virtio DT node at all (foreign platform or
			 * DTB stripped). Never rescue here: permanent no-op. */
			pr_info("no trusty DT node, staying out forever\n");
			return;
		}
		if (dev) {
			platform_device_put(dev);
			if (rescued)
				pr_info("device present after rescue (expected)\n");
			else
				pr_info("trusty-virtio present (gz healthy) at +%ds - no-op\n", i);
			return;
		}

		/* Device absent so far. Is the driver registered? */
		{
			struct device_driver *drv =
				driver_find(VIRTIO_DRV_NAME, &platform_bus_type);

			if (drv) {
				put_device(&drv->p);
				if (drv_seen_at < 0)
					drv_seen_at = i;
			} else {
				/* Driver not loaded yet (modules still coming up) or
				 * already cut by us earlier (then device exists and we
				 * would have returned above). Reset the grace clock. */
				drv_seen_at = -1;
			}
		}

		if (drv_seen_at >= 0 && (i - drv_seen_at) >= GRACE_SECS) {
			pr_info("driver up %ds, device still absent -> gz-erased fingerprint, rescuing\n",
				i - drv_seen_at);
			if (do_rescue() != -EAGAIN) {
				rescued = true;
				return;
			}
			drv_seen_at = -1;   /* parent not ready; keep polling */
		}
		msleep(1000);
	}

	pr_warn("gave up after %ds (driver never settled absent) - stayed safe\n",
		settle_secs);
}
static DECLARE_DELAYED_WORK(trusty_rescue_work, rescue_work);

static int __init trusty_rescue_init(void)
{
	/* Start after first-stage init hands off to vendor_dlkm module loading;
	 * the work item keeps polling anyway, so the exact delay is not critical. */
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
