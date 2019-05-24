// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Arm Ltd.

#define pr_fmt(fmt) "mpam: " fmt

#include <linux/arm_mpam.h>
#include <linux/bitmap.h>
#include <linux/bits.h>
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <asm/mpam.h>

#include "mpam_internal.h"

/*
 * During discovery this lock protects writers to class, components and devices.
 * Once all devices are successfully probed, the system_supports_mpam() static
 * key is enabled, and these lists become read only.
 */
static DEFINE_MUTEX(mpam_devices_lock);
/* Devices are MSCs */
static LIST_HEAD(mpam_all_devices);

/* Classes are the set of MSCs that make up components of the same type. */
LIST_HEAD(mpam_classes_rcu);

/* Hold this when registering/unregistering cpuhp callbacks */
static DEFINE_MUTEX(mpam_cpuhp_lock);
static int mpam_cpuhp_state;

struct mpam_sysprops mpam_sysprops;

/*
 * mpam is enabled once all devices have been probed from CPU online callbacks,
 * scheduled via this work_struct.
 */
static struct work_struct mpam_enable_work;

struct mpam_device_cfg_update
{
	struct mpam_class *class;
	struct mpam_component *comp;

	/* cfg is NULL for a reset */
	struct mpam_component_cfg_update *cfg;

	/*
	 * If the device is reachable from one of these cpus, it has been
	 * updated.
	 */
	struct cpumask updated_on;
	int first_error;
};

static inline u32 mpam_read_reg(struct mpam_device *dev, u16 reg)
{
	WARN_ON_ONCE(reg > SZ_MPAM_DEVICE);
	assert_spin_locked(&dev->lock);

	/*
	 * If we touch a device that isn't accessible from this CPU we may get
	 * an external-abort.
	 */
	WARN_ON_ONCE(preemptible());
	WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), &dev->fw_affinity));

	return readl_relaxed(dev->mapped_hwpage + reg);
}

static inline void mpam_write_reg(struct mpam_device *dev, u16 reg, u32 val)
{
	WARN_ON_ONCE(reg > SZ_MPAM_DEVICE);
	assert_spin_locked(&dev->lock);

	/*
	 * If we touch a device that isn't accessible from this CPU we may get
	 * an external-abort. If we're lucky, we corrupt another mpam:component.
	 */
	WARN_ON_ONCE(preemptible());
	WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), &dev->fw_affinity));

	writel_relaxed(val, dev->mapped_hwpage + reg);
}

static struct mpam_device * __init
mpam_device_alloc(struct mpam_component *comp)
{
	struct mpam_device *dev;

	lockdep_assert_held(&mpam_devices_lock);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&dev->lock);
	INIT_LIST_HEAD(&dev->comp_list);
	INIT_LIST_HEAD(&dev->glbl_list);

	dev->comp = comp;
	list_add(&dev->comp_list, &comp->devices);
	list_add(&dev->glbl_list, &mpam_all_devices);

	return dev;
}

static void mpam_devices_destroy(struct mpam_component *comp)
{
	struct mpam_device *dev, *tmp;

	lockdep_assert_held(&mpam_devices_lock);

	list_for_each_entry_safe(dev, tmp, &comp->devices, comp_list) {
		list_del(&dev->comp_list);
		list_del(&dev->glbl_list);
		kfree(dev);
	}
}

static struct mpam_component * __init mpam_component_alloc(int id)
{
	struct mpam_component *comp;

	comp = kzalloc(sizeof(*comp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&comp->devices);
	INIT_LIST_HEAD(&comp->resctrl_domain.list);
	INIT_LIST_HEAD(&comp->class_list);

	comp->resctrl_domain.id = id;

	return comp;
}

struct mpam_component *mpam_component_get(struct mpam_class *class, int id,
					  bool alloc)
{
	struct mpam_component *comp;

	list_for_each_entry(comp, &class->components, class_list) {
		if (comp->resctrl_domain.id == id)
			return comp;
	}

	if (!alloc)
		return ERR_PTR(-ENOENT);

	comp = mpam_component_alloc(id);
	if (IS_ERR(comp))
		return comp;

	list_add(&comp->class_list, &class->components);

	return comp;
}

/* free all components and devices of this class */
static void mpam_class_destroy(struct mpam_class *class)
{
	struct mpam_component *comp, *tmp;

	lockdep_assert_held(&mpam_devices_lock);

	list_for_each_entry_safe(comp, tmp, &class->components, class_list) {
		mpam_devices_destroy(comp);
		list_del(&comp->class_list);
		kfree(comp);
	}
}

static struct mpam_class * __init mpam_class_alloc(u8 level_idx, enum mpam_class_types type)
{
	struct mpam_class *class;

	lockdep_assert_held(&mpam_devices_lock);

	class = kzalloc(sizeof(*class), GFP_KERNEL);
	if (!class)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&class->components);
	INIT_LIST_HEAD(&class->resctrl_res.domains);
	INIT_LIST_HEAD(&class->classes_list_rcu);

	/* even if its not a cache: */
	class->resctrl_res.cache_level = level_idx;
	class->type = type;

	list_add_rcu(&class->classes_list_rcu, &mpam_classes_rcu);

	return class;
}

static struct mpam_class * __init mpam_class_get(u8 level_idx,
						 enum mpam_class_types type,
						 bool alloc)
{
	bool found = false;
	struct mpam_class *class;

	pr_debug("mpam_class_get(%d)\n", level_idx);

	rcu_read_lock();
	list_for_each_entry_rcu(class, &mpam_classes_rcu, classes_list_rcu) {
		if (class->type == type &&
		    class->resctrl_res.cache_level == level_idx) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();

	if (found)
		return class;

	if (!alloc)
		return ERR_PTR(-ENOENT);

	return mpam_class_alloc(level_idx, type);
}

/*
 * Create a a device with this @hwpage_address, of class type:level_idx.
 * class/component structures may be allocated.
 * Returns the new device, or an ERR_PTR().
 */
struct mpam_device * __init
__mpam_device_create(u8 level_idx, enum mpam_class_types type,
		     int component_id, const struct cpumask *fw_affinity,
		     phys_addr_t hwpage_address)
{
	struct mpam_device *dev;
	struct mpam_class *class;
	struct mpam_component *comp;

	if (!fw_affinity)
		fw_affinity = cpu_possible_mask;

	mutex_lock(&mpam_devices_lock);
	do {
		class = mpam_class_get(level_idx, type, true);
		if (IS_ERR(class)) {
			dev = (void *)class;
			break;
		}

		comp = mpam_component_get(class, component_id, true);
		if (IS_ERR(comp)) {
			dev = (void *)comp;
			break;
		}

		/*
		 * For caches we learn the affinity from the cache-id as CPUs
		 * come online. For everything else, we have to be told.
		 */
		if (type != MPAM_CLASS_CACHE)
			cpumask_or(&comp->fw_affinity, &comp->fw_affinity, fw_affinity);

		dev = mpam_device_alloc(comp);
		if (IS_ERR(dev))
			break;

		dev->fw_affinity = *fw_affinity;
		dev->hwpage_address = hwpage_address;
		dev->mapped_hwpage = ioremap(hwpage_address, SZ_MPAM_DEVICE);
		if (!dev->mapped_hwpage)
			dev = ERR_PTR(-ENOMEM);
	} while (0);
	mutex_unlock(&mpam_devices_lock);

	return dev;
}

void __init mpam_device_set_error_irq(struct mpam_device *dev, u32 irq,
				      u32 flags)
{
	unsigned long irq_save_flags;

	spin_lock_irqsave(&dev->lock, irq_save_flags);
	dev->error_irq = irq;
	dev->error_irq_flags = flags & MPAM_IRQ_FLAGS_MASK;
	spin_unlock_irqrestore(&dev->lock, irq_save_flags);
}

void __init mpam_device_set_overflow_irq(struct mpam_device *dev, u32 irq,
					 u32 flags)
{
	unsigned long irq_save_flags;

	spin_lock_irqsave(&dev->lock, irq_save_flags);
	dev->overflow_irq = irq;
	dev->overflow_irq_flags = flags & MPAM_IRQ_FLAGS_MASK;
	spin_unlock_irqrestore(&dev->lock, irq_save_flags);
}

static void mpam_probe_update_sysprops(u16 max_partid, u8 max_pmg)
{
	lockdep_assert_held(&mpam_devices_lock);

	mpam_sysprops.max_partid = min(mpam_sysprops.max_partid, max_partid);
	mpam_sysprops.max_pmg = min(mpam_sysprops.max_pmg, max_pmg);
}

static int mpam_device_probe(struct mpam_device *dev)
{
	u32 hwfeatures;
	u16 max_partid, max_pmg;

	if (mpam_read_reg(dev, MPAMF_AIDR) != MPAM_ARCHITECTURE_V1) {
		pr_err_once("device at 0x%llx does not match MPAM architecture v1.0\n",
			    dev->hwpage_address);
		return -EIO;
	}

	hwfeatures = mpam_read_reg(dev, MPAMF_IDR);
	max_partid = hwfeatures & MPAMF_IDR_PARTID_MAX_MASK;
	max_pmg = (hwfeatures & MPAMF_IDR_PMG_MAX_MASK) >> MPAMF_IDR_PMG_MAX_SHIFT;

	mpam_probe_update_sysprops(max_partid, max_pmg);

	/* Cache Capacity Partitioning */
	if (hwfeatures & MPAMF_IDR_HAS_CCAP_PART) {
		u32 ccap_features = mpam_read_reg(dev, MPAMF_CCAP_IDR);

		pr_debug("probe: probed CCAP_PART\n");

		dev->cmax_wd = ccap_features & MPAMF_CCAP_IDR_CMAX_WD;
		if (dev->cmax_wd)
			mpam_set_feature(mpam_feat_ccap_part, &dev->features);

	}

	/* Cache Portion partitioning */
	if (hwfeatures & MPAMF_IDR_HAS_CPOR_PART) {
		u32 cpor_features = mpam_read_reg(dev, MPAMF_CPOR_IDR);

		pr_debug("probe: probed CPOR_PART\n");

		dev->cpbm_wd = cpor_features & MPAMF_CPOR_IDR_CPBM_WD;
		if (dev->cpbm_wd)
			mpam_set_feature(mpam_feat_cpor_part, &dev->features);
	}

	/* Memory bandwidth partitioning */
	if (hwfeatures & MPAMF_IDR_HAS_MBW_PART) {
		u32 mbw_features = mpam_read_reg(dev, MPAMF_MBW_IDR);

		pr_debug("probe: probed MBW_PART\n");

		/* portion bitmap resolution */
		dev->mbw_pbm_bits = (mbw_features & MPAMF_MBW_IDR_BWPBM_WD) >>
				     MPAMF_MBW_IDR_BWPBM_WD_SHIFT;
		if (dev->mbw_pbm_bits && (mbw_features & MPAMF_MBW_IDR_HAS_PBM))
			mpam_set_feature(mpam_feat_mbw_part, &dev->features);

		dev->bwa_wd = (mbw_features & MPAMF_MBW_IDR_BWA_WD);
		if (dev->bwa_wd && (mbw_features & MPAMF_MBW_IDR_HAS_MAX))
			mpam_set_feature(mpam_feat_mbw_max, &dev->features);

		if (dev->bwa_wd && (mbw_features & MPAMF_MBW_IDR_HAS_MIN))
			mpam_set_feature(mpam_feat_mbw_min, &dev->features);

		if (dev->bwa_wd && (mbw_features & MPAMF_MBW_IDR_HAS_PROP))
			mpam_set_feature(mpam_feat_mbw_prop, &dev->features);

	}

	/* Priority partitioning */
	if (hwfeatures & MPAMF_IDR_HAS_PRI_PART) {
		u32 pri_features = mpam_read_reg(dev, MPAMF_PRI_IDR);

		pr_debug("probe: probed PRI_PART\n");

		dev->intpri_wd = (pri_features & MPAMF_PRI_IDR_INTPRI_WD) >>
				  MPAMF_PRI_IDR_INTPRI_WD_SHIFT;
		if (dev->intpri_wd && (pri_features & MPAMF_PRI_IDR_HAS_INTPRI)) {
			mpam_set_feature(mpam_feat_intpri_part, &dev->features);
			if (pri_features & MPAMF_PRI_IDR_INTPRI_0_IS_LOW)
				mpam_set_feature(mpam_feat_intpri_part_0_low, &dev->features);
		}

		dev->dspri_wd = (pri_features & MPAMF_PRI_IDR_DSPRI_WD) >>
				  MPAMF_PRI_IDR_DSPRI_WD_SHIFT;
		if (dev->dspri_wd && (pri_features & MPAMF_PRI_IDR_HAS_DSPRI)) {
			mpam_set_feature(mpam_feat_dspri_part, &dev->features);
			if (pri_features & MPAMF_PRI_IDR_DSPRI_0_IS_LOW)
				mpam_set_feature(mpam_feat_dspri_part_0_low, &dev->features);
		}
	}

	/* Performance Monitoring */
	if (hwfeatures & MPAMF_IDR_HAS_MSMON) {
		u32 msmon_features = mpam_read_reg(dev, MPAMF_MSMON_IDR);

		pr_debug("probe: probed MSMON\n");

		if (msmon_features & MPAMF_MSMON_IDR_MSMON_CSU) {
			u32 csumonidr;

			csumonidr = mpam_read_reg(dev, MPAMF_CSUMON_IDR);
			dev->num_csu_mon = csumonidr & MPAMF_CSUMON_IDR_NUM_MON;
			if (dev->num_csu_mon)
				mpam_set_feature(mpam_feat_msmon_csu,
						 &dev->features);
		}
		if (msmon_features & MPAMF_MSMON_IDR_MSMON_MBWU) {
			u32 mbwumonidr = mpam_read_reg(dev, MPAMF_MBWUMON_IDR);

			dev->num_mbwu_mon = mbwumonidr & MPAMF_MBWUMON_IDR_NUM_MON;
			if (dev->num_mbwu_mon)
				mpam_set_feature(mpam_feat_msmon_mbwu,
						 &dev->features);

		}
	}

	dev->probed = true;

	return 0;
}

/*
 * If device doesn't match class feature/configuration, do the right thing.
 * For 'num' properties we can just take the minimum.
 * For properties where the mismatched unused bits would make a difference, we
 * nobble the class feature, as we can't configure all the devices.
 * e.g. The L3 cache is composed of two devices with 13 and 17 portion
 * bitmaps respectively.
 */
static void __device_class_feature_mismatch(struct mpam_device *dev,
					    struct mpam_class *class)
{
	lockdep_assert_held(&mpam_devices_lock); /* we modify class */

	if (class->cpbm_wd != dev->cpbm_wd)
		mpam_clear_feature(mpam_feat_cpor_part, &class->features);
	if (class->mbw_pbm_bits != dev->mbw_pbm_bits)
		mpam_clear_feature(mpam_feat_mbw_part, &class->features);

	/* For num properties, take the minimum */
	if (class->num_csu_mon != dev->num_csu_mon)
		class->num_csu_mon = min(class->num_csu_mon, dev->num_csu_mon);
	if (class->num_mbwu_mon != dev->num_mbwu_mon)
		class->num_mbwu_mon = min(class->num_mbwu_mon, dev->num_mbwu_mon);

	/* bwa_wd is a count of bits, fewer bits means less precision */
	if (class->bwa_wd != dev->bwa_wd)
		class->bwa_wd = min(class->bwa_wd, dev->bwa_wd);

	if (class->intpri_wd != dev->intpri_wd)
		class->intpri_wd = min(class->intpri_wd, dev->intpri_wd);
	if (class->dspri_wd != dev->dspri_wd)
		class->dspri_wd = min(class->dspri_wd, dev->dspri_wd);

	/* {int,ds}pri may not have differing 0-low behaviour */
	if (mpam_has_feature(mpam_feat_intpri_part_0_low, class->features) !=
	    mpam_has_feature(mpam_feat_intpri_part_0_low, dev->features))
		mpam_clear_feature(mpam_feat_intpri_part, &class->features);
	if (mpam_has_feature(mpam_feat_dspri_part_0_low, class->features) !=
	    mpam_has_feature(mpam_feat_dspri_part_0_low, dev->features))
		mpam_clear_feature(mpam_feat_dspri_part, &class->features);
}

/*
 * Squash common class=>component=>device->features down to the
 * class->features
 */
static void mpam_enable_squash_features(void)
{
	unsigned long flags;
	struct mpam_device *dev;
	struct mpam_class *class;
	struct mpam_component *comp;

	rcu_read_lock();
	list_for_each_entry_rcu(class, &mpam_classes_rcu, classes_list_rcu) {
		/*
		 * Copy the first component's first device's properties and
		 * features to the class. __device_class_feature_mismatch()
		 * will fix them as appropriate.
		 * It is not possible to have a component with no devices.
		 */
		if (!list_empty(&class->components)) {
			comp = list_first_entry_or_null(&class->components,
						       struct mpam_component,
						       class_list);
			if (WARN_ON(!comp))
				break;

			dev = list_first_entry_or_null(&comp->devices,
						       struct mpam_device,
						       comp_list);
			if (WARN_ON(!dev))
				break;

			spin_lock_irqsave(&dev->lock, flags);
			class->features = dev->features;
			class->cpbm_wd = dev->cpbm_wd;
			class->mbw_pbm_bits = dev->mbw_pbm_bits;
			class->bwa_wd = dev->bwa_wd;
			class->intpri_wd = dev->intpri_wd;
			class->dspri_wd = dev->dspri_wd;
			class->num_csu_mon = dev->num_csu_mon;
			class->num_mbwu_mon = dev->num_mbwu_mon;
			spin_unlock_irqrestore(&dev->lock, flags);
		}

		list_for_each_entry(comp, &class->components, class_list) {
			list_for_each_entry(dev, &comp->devices, comp_list) {
				spin_lock_irqsave(&dev->lock, flags);
				__device_class_feature_mismatch(dev, class);
				class->features &= dev->features;
				spin_unlock_irqrestore(&dev->lock, flags);
			}
		}
	}
	rcu_read_unlock();
}

static const char *mpam_msc_err_str[_MPAM_NUM_ERRCODE] = {
	[MPAM_ERRCODE_NONE] = "No Error",
	[MPAM_ERRCODE_PARTID_SEL_RANGE] = "Out of range PARTID selected",
	[MPAM_ERRCODE_REQ_PARTID_RANGE] = "Out of range PARTID requested",
	[MPAM_ERRCODE_REQ_PMG_RANGE] = "Out of range PMG requested",
	[MPAM_ERRCODE_MONITOR_RANGE] = "Out of range Monitor selected",
	[MPAM_ERRCODE_MSMONCFG_ID_RANGE] = "Out of range Monitor:PARTID or PMG written",

	/* These two are about PARTID narrowing, which we don't support */
	[MPAM_ERRCODE_INTPARTID_RANGE] = "Out or range Internal-PARTID written",
	[MPAM_ERRCODE_UNEXPECTED_INTERNAL] = "Internal-PARTID set but not expected",
};


static irqreturn_t mpam_handle_error_irq(int irq, void *data)
{
	u32 device_esr;
	u16 device_errcode;
	struct mpam_device *dev = data;

	spin_lock(&dev->lock);
	device_esr = mpam_read_reg(dev, MPAMF_ESR);
	spin_unlock(&dev->lock);

	device_errcode = (device_esr & MPAMF_ESR_ERRCODE) >> MPAMF_ESR_ERRCODE_SHIFT;
	if (device_errcode == MPAM_ERRCODE_NONE)
		return IRQ_NONE;

	/* No-one expects MPAM errors! */
	if (device_errcode <= _MPAM_NUM_ERRCODE)
		pr_err_ratelimited("unexpected error '%s' [esr:%x]\n",
				   mpam_msc_err_str[device_errcode],
				   device_esr);
	else
		pr_err_ratelimited("unexpected error %d [esr:%x]\n",
				   device_errcode, device_esr);

	/* A write of 0 to MPAMF_ESR.ERRCODE clears level interrupts */
	spin_lock(&dev->lock);
	mpam_write_reg(dev, MPAMF_ESR, 0);
	spin_unlock(&dev->lock);

	return IRQ_HANDLED;
}

/* register and enable all device error interrupts */
static void mpam_enable_irqs(void)
{
	struct mpam_device *dev;
	int rc, irq, request_flags;
	unsigned long irq_save_flags;

	list_for_each_entry(dev, &mpam_all_devices, glbl_list) {
		spin_lock_irqsave(&dev->lock, irq_save_flags);
		irq = dev->error_irq;
		request_flags = dev->error_irq_flags;
		spin_unlock_irqrestore(&dev->lock, irq_save_flags);

		if (request_flags & MPAM_IRQ_MODE_LEVEL) {
			struct cpumask tmp;
			bool inaccessible_cpus;

			request_flags = IRQF_TRIGGER_LOW | IRQF_SHARED;

			/*
			 * If the MSC is not accessible from any CPU the IRQ
			 * may be migrated to, we won't be able to clear it.
			 * ~dev->fw_affinity is all the CPUs that can't access
			 * the MSC. 'and' cpu_possible_mask tells us whether we
			 * care.
			 */
			spin_lock_irqsave(&dev->lock, irq_save_flags);
			inaccessible_cpus = cpumask_andnot(&tmp,
							  cpu_possible_mask,
							  &dev->fw_affinity);
			spin_unlock_irqrestore(&dev->lock, irq_save_flags);

			if (inaccessible_cpus) {
				pr_err_once("NOT registering MPAM error level-irq that isn't globally reachable");
				continue;
			}
		} else {
			request_flags = IRQF_TRIGGER_RISING | IRQF_SHARED;
		}

		rc = request_irq(irq, mpam_handle_error_irq, request_flags,
				 "MPAM ERR IRQ", dev);
		if (rc) {
			pr_err_ratelimited("Failed to register irq %u\n", irq);
			continue;
		}

		/*
		 * temporary: the interrupt will only be enabled when cpus
		 * subsequently come online after mpam_enable().
		 */
		spin_lock_irqsave(&dev->lock, irq_save_flags);
		dev->enable_error_irq = true;
		spin_unlock_irqrestore(&dev->lock, irq_save_flags);
	}
}

/*
 * Enable mpam once all devices have been probed.
 * Scheduled by mpam_discovery_complete() once all devices have been created.
 * Also scheduled when new devices are probed when new CPUs come online.
 */
static void mpam_enable(struct work_struct *work)
{
	unsigned long flags;
	struct mpam_device *dev;
	bool all_devices_probed = true;

	/* Have we probed all the devices? */
	mutex_lock(&mpam_devices_lock);
	list_for_each_entry(dev, &mpam_all_devices, glbl_list) {
		spin_lock_irqsave(&dev->lock, flags);
		if (!dev->probed)
			all_devices_probed = false;
		spin_unlock_irqrestore(&dev->lock, flags);

		if (!all_devices_probed)
			break;
	}
	mutex_unlock(&mpam_devices_lock);

	if (!all_devices_probed)
		return;

	mutex_lock(&mpam_devices_lock);
	mpam_enable_squash_features();
	mpam_enable_irqs();
	mutex_unlock(&mpam_devices_lock);

	mpam_resctrl_init();
}


int __init mpam_discovery_start(void)
{
	if (!mpam_cpus_have_feature())
		return -EOPNOTSUPP;

	mpam_sysprops.max_partid = mpam_cpu_max_partids();
	mpam_sysprops.max_pmg = mpam_cpu_max_pmgs();

	INIT_WORK(&mpam_enable_work, mpam_enable);

	return 0;
}

static void mpam_reset_device_bitmap(struct mpam_device *dev, u16 reg, u16 wd)
{
	u32 bm = ~0;
	int i;

	lockdep_assert_held(&dev->lock);

	/* write all but the last full-32bit-word */
	for (i = 0; i < wd / 32; i++, reg += sizeof(bm)) {
		mpam_write_reg(dev, reg, bm);
	}

	/* and the last partial 32bit word */
	bm = GENMASK(wd % 32, 0);
	if (bm)
		mpam_write_reg(dev, reg, bm);
}

static void mpam_reset_device_partid(struct mpam_device *dev, u16 partid)
{
	u16 cmax = GENMASK(dev->cmax_wd, 0);
	u16 bwa_fract = GENMASK(15, dev->bwa_wd);
	u16 intpri = GENMASK(dev->intpri_wd, 0);
	u16 dspri = GENMASK(dev->dspri_wd, 0);
	u32 pri_val = 0;

	lockdep_assert_held(&dev->lock);

	if (!mpam_has_part_sel(dev->features))
		return;

	mpam_write_reg(dev, MPAMCFG_PART_SEL, partid);
	wmb(); /* subsequent writes must be applied to our new partid */

	if (mpam_has_feature(mpam_feat_ccap_part, dev->features))
		mpam_write_reg(dev, MPAMCFG_CMAX, cmax);

	if (mpam_has_feature(mpam_feat_cpor_part, dev->features))
		mpam_reset_device_bitmap(dev, MPAMCFG_CPBM, dev->cpbm_wd);

	if (mpam_has_feature(mpam_feat_mbw_part, dev->features))
		mpam_reset_device_bitmap(dev, MPAMCFG_MBW_PBM, dev->mbw_pbm_bits);

	if (mpam_has_feature(mpam_feat_mbw_min, dev->features))
		mpam_write_reg(dev, MPAMCFG_MBW_MIN, bwa_fract);

	if (mpam_has_feature(mpam_feat_mbw_max, dev->features))
		mpam_write_reg(dev, MPAMCFG_MBW_MAX, bwa_fract);

	if (mpam_has_feature(mpam_feat_mbw_prop, dev->features))
		mpam_write_reg(dev, MPAMCFG_MBW_PROP, bwa_fract);

	if (mpam_has_feature(mpam_feat_intpri_part, dev->features) ||
	    mpam_has_feature(mpam_feat_dspri_part, dev->features)) {
		/* aces high? */
		if (!mpam_has_feature(mpam_feat_intpri_part_0_low,
				      dev->features))
			intpri = 0;
		if (!mpam_has_feature(mpam_feat_dspri_part_0_low,
				      dev->features))
			dspri = 0;

		if (mpam_has_feature(mpam_feat_intpri_part, dev->features))
			pri_val |= intpri;
		if (mpam_has_feature(mpam_feat_dspri_part, dev->features))
			pri_val |= (dspri << MPAMCFG_PRI_DSPRI_SHIFT);

		mpam_write_reg(dev, MPAMCFG_PRI, pri_val);
	}

	mb(); /* complete the configuration before the cpu can use this partid */
}

/*
 * Apply the specified component config to this device.
 */
static int __apply_config(struct mpam_device *dev,
			  struct mpam_component_cfg_update *arg)
{
	u16 reg;

	lockdep_assert_held(&dev->lock);

	if (!mpam_has_feature(arg->feat, dev->features))
		return -EOPNOTSUPP;
	if (!arg->mpam_cfg) {
		pr_err_ratelimited("Refusing empty configuration");
		return -EINVAL;
	}

	switch (arg->feat) {
	case mpam_feat_mbw_max:
		reg = MPAMCFG_MBW_MAX;
		break;
	case mpam_feat_cpor_part:
		reg = MPAMCFG_CPBM;
		break;
	case mpam_feat_mbw_part:
		reg = MPAMCFG_MBW_PBM;
		break;
	default:
		pr_err_ratelimited("Configuration attempt for unknown feature\n");
		return -EIO;
	}

	mpam_write_reg(dev, MPAMCFG_PART_SEL, arg->partid);
	wmb(); /* subsequent writes must be applied to our new partid */

	mpam_write_reg(dev, reg, arg->mpam_cfg);
	mb(); /* complete the configuration before the cpu can use this partid */

	return 0;
}

/*
 * Called from cpuhp callbacks and with the cpus_read_lock() held from
 * mpam_reset_devices().
 */
static void mpam_reset_device(struct mpam_class *class, struct mpam_component *comp,
			      struct mpam_device *dev)
{
	int err;
	u16 partid;
	struct mpam_component_cfg_update cfg;
	struct mpam_component_cfg_update *cfg_p;

	lockdep_assert_held(&dev->lock);

	if (dev->enable_error_irq)
		mpam_write_reg(dev, MPAMF_ECR, MPAMF_ECR_INTEN);

	for (partid = 0; partid < mpam_sysprops.max_partid; partid++) {
		mpam_reset_device_partid(dev, partid);

		/*
		 * If cpuhp is driving the reset, we need to retrieve the
		 * resctrl config if there is one.
		 */
		cfg_p = mpam_resctrl_get_converted_config(class, comp, partid,
							  &cfg);
		if (cfg_p) {
			/* An error here leaves the reset config in place */
			err = __apply_config(dev, cfg_p);
			if (err)
				pr_warn_once("Failed to apply resctrl config during reset");
		}
	}
}

static int mpam_device_apply_config(struct mpam_device *dev,
				    struct mpam_device_cfg_update *cfg_update)
{
	int ret = 0;
	unsigned long flags;
	struct mpam_component_cfg_update *cfg = cfg_update->cfg;

	spin_lock_irqsave(&dev->lock, flags);
	if (cfg)
		ret = __apply_config(dev, cfg);
	else
		mpam_reset_device(cfg_update->class, cfg_update->comp, dev);
	spin_unlock_irqrestore(&dev->lock, flags);

	return ret;
}

/* Update all newly reachable devices. Call with cpus_read_lock() held. */
static void mpam_component_apply_all_local(void *d)
{
	int err;
	struct mpam_device *dev;
	struct mpam_device_cfg_update *cfg_update = d;
	struct mpam_component *comp = cfg_update->comp;

	list_for_each_entry(dev, &comp->devices, comp_list) {
		if (cpumask_intersects(&dev->online_affinity,
				       &cfg_update->updated_on))
			continue;

		/* This device needs updating, can I reach it? */
		if (!cpumask_test_cpu(smp_processor_id(), &dev->online_affinity))
			continue;

		/* Apply new configuration to this device */
		err = mpam_device_apply_config(dev, cfg_update);
		if (err)
			cmpxchg(&cfg_update->first_error, 0, err);
	}

	cpumask_set_cpu(smp_processor_id(), &cfg_update->updated_on);
}

/* Call with cpuhp lock held */
int mpam_component_apply_all(struct mpam_class *class,
			     struct mpam_component *comp,
			     struct mpam_component_cfg_update *cfg)
{
	int cpu;
	struct mpam_device *dev;
	struct mpam_device_cfg_update cfg_update;

	/* The online_affinity masks must not change while we do this */
	lockdep_assert_cpus_held();

	cfg_update.class = class;
	cfg_update.comp =  comp;
	cfg_update.cfg = cfg;
	cfg_update.first_error = 0;
	cpumask_clear(&cfg_update.updated_on);

	cpu = get_cpu();
	/* Update any devices we can reach locally */
	if (cpumask_test_cpu(cpu, &comp->fw_affinity))
		mpam_component_apply_all_local(&cfg_update);
	put_cpu();

	/* Find the set of other CPUs we need to run on to update this component */
	list_for_each_entry(dev, &comp->devices, comp_list) {
		if (cfg_update.first_error)
			break;

		if (cpumask_intersects(&dev->online_affinity,
				       &cfg_update.updated_on))
			continue;

		/*
		 * This device needs the config applying, and hasn't been
		 * reachable by any cpu so far.
		 */
		cpu = cpumask_any(&dev->online_affinity);
		smp_call_function_single(cpu, mpam_component_apply_all_local,
					 &cfg_update, 1);
	}

	return cfg_update.first_error;
}

/*
 * Reset every component, configuring every partid unrestricted.
 * Call with cpuhp lock held.
 */
void mpam_reset_devices(void)
{
	struct mpam_class *class;
	struct mpam_component *comp;

	lockdep_assert_cpus_held();

	mutex_lock(&mpam_devices_lock);
	rcu_read_lock();
	list_for_each_entry_rcu(class, &mpam_classes_rcu, classes_list_rcu) {
		list_for_each_entry(comp, &class->components, class_list)
			mpam_component_apply_all(class, comp, NULL);
	}
	rcu_read_unlock();
	mutex_unlock(&mpam_devices_lock);
}

/*
 * Firmware didn't give us an affinity, but a cache-id, if this cpu has that
 * cache-id, update the fw_affinity for this component.
 */
static void mpam_sync_cpu_cache_component_fw_affinity(struct mpam_class *class,
						      int cpu)
{
	u8 level;
	int cpu_cache_id;
	struct cacheinfo *leaf;
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_devices_lock); /* we modify mpam_sysprops */

	if (class->type != MPAM_CLASS_CACHE)
		return;

	level = class->resctrl_res.cache_level;
	cpu_cache_id = get_cpu_cacheinfo_id(cpu, level);
	comp = mpam_component_get(class, cpu_cache_id, false);

	/* This cpu does not have a component of this class */
	if (!comp)
		return;

	/*
	 * The resctrl rmid_threshold is based on cache size. Keep track of
	 * the biggest cache we've seen.
	 */
	leaf = get_cpu_cache_leaf(cpu, level);
	if (leaf)
		mpam_sysprops.mpam_llc_size = max(mpam_sysprops.mpam_llc_size,
						  leaf->size);

	cpumask_set_cpu(cpu, &comp->fw_affinity);
	cpumask_set_cpu(cpu, &class->fw_affinity);
}

static int __online_devices(struct mpam_class *class,
			    struct mpam_component *comp, int cpu)
{
	int err = 0;
	unsigned long flags;
	struct mpam_device *dev;
	bool new_device_probed = false;

	list_for_each_entry(dev, &comp->devices, comp_list) {
		if (!cpumask_test_cpu(cpu, &dev->fw_affinity))
			continue;

		spin_lock_irqsave(&dev->lock, flags);
		if (!dev->probed) {
			err = mpam_device_probe(dev);
			if (!err)
				new_device_probed = true;
		}

		if (cpumask_empty(&dev->online_affinity))
			mpam_reset_device(class, comp, dev);

		cpumask_set_cpu(cpu, &dev->online_affinity);
		spin_unlock_irqrestore(&dev->lock, flags);

		if (err)
			return err;
	}

	if (new_device_probed)
		return 1;

	return 0;
}

static int mpam_cpu_online(unsigned int cpu)
{
	int err = 0;
	struct mpam_class *class;
	struct mpam_component *comp;
	bool new_device_probed = false;

	mutex_lock(&mpam_devices_lock);
	rcu_read_lock();
	list_for_each_entry_rcu(class, &mpam_classes_rcu, classes_list_rcu) {
		mpam_sync_cpu_cache_component_fw_affinity(class, cpu);

		list_for_each_entry(comp, &class->components, class_list) {
			if (!cpumask_test_cpu(cpu, &comp->fw_affinity))
				continue;

			err = __online_devices(class, comp, cpu);
			if (err > 0)
				new_device_probed = true;
			if (err < 0)
				break; // mpam_broken
		}
	}
	rcu_read_unlock();

	if (new_device_probed && err >= 0)
		schedule_work(&mpam_enable_work);

	mutex_unlock(&mpam_devices_lock);

	if (err < 0)
		return err;

	mpam_resctrl_cpu_online(cpu);

	return 0;
}

static int mpam_cpu_offline(unsigned int cpu)
{
	unsigned long flags;
	struct mpam_device *dev;

	mutex_lock(&mpam_devices_lock);
	list_for_each_entry(dev, &mpam_all_devices, glbl_list){
		if (!cpumask_test_cpu(cpu, &dev->online_affinity))
			continue;

		cpumask_clear_cpu(cpu, &dev->online_affinity);

		if (cpumask_empty(&dev->online_affinity)) {
			spin_lock_irqsave(&dev->lock, flags);
			mpam_write_reg(dev, MPAMF_ECR, 0);
			spin_unlock_irqrestore(&dev->lock, flags);
		}
	}
	mutex_unlock(&mpam_devices_lock);

	mpam_resctrl_cpu_offline(cpu);

	return 0;
}

void __init mpam_discovery_complete(void)
{
	mutex_lock(&mpam_cpuhp_lock);
	mpam_cpuhp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
					     "mpam:online", mpam_cpu_online,
					     mpam_cpu_offline);
	if (mpam_cpuhp_state <= 0)
		pr_err("Failed to register 'dyn' cpuhp callbacks");
	mutex_unlock(&mpam_cpuhp_lock);
}

void mpam_discovery_failed(void)
{
	struct mpam_class *class;

	mutex_lock(&mpam_devices_lock);
	rcu_read_lock();
	list_for_each_entry_rcu(class, &mpam_classes_rcu, classes_list_rcu)
		mpam_class_destroy(class);
	rcu_read_unlock();
	mutex_unlock(&mpam_devices_lock);
}

