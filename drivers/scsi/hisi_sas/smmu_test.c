#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/dmapool.h>
#include <linux/iopoll.h>
#include <linux/lcm.h>
#include <linux/libata.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/timer.h>
#include <scsi/sas_ata.h>
#include <scsi/libsas.h>
#include <linux/kthread.h>

#define TIMER_PERIOD_SECONDS 30

static int ways = 64;
module_param(ways, int, S_IRUGO);

static int seconds = 10;
module_param(seconds, int, S_IRUGO);

static int completions = 2000;
module_param(completions, int, S_IRUGO);


unsigned long long mappings[NR_CPUS];
struct semaphore sem[NR_CPUS+1];


extern struct device *get_zip_dev(void);

#define COMPLETIONS_SIZE 2000

static noinline dma_addr_t test_mapsingle(struct device *dev, void *buf, int size)
{
	dma_addr_t dma_addr = dma_map_single(dev, buf, size, DMA_TO_DEVICE);
	return dma_addr;
}

static noinline void test_unmapsingle(struct device *dev, void *buf, int size, dma_addr_t dma_addr)
{
	dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);
}

static noinline void test_memcpy(void *out, void *in, int size)
{  
	memcpy(out, in, size);
}

extern struct device *hisi_sas_dev;

static int testthread(void *data)
{  
	unsigned long stop = jiffies +seconds*HZ;
	struct device *dev = hisi_sas_dev;
	char **inputs;
	char **outputs;
	dma_addr_t *dma_addrs;
	int i, cpu = smp_processor_id();
	struct semaphore *sem = data;

	inputs = kcalloc(completions, sizeof(char *), GFP_KERNEL);
	outputs = kcalloc(completions, sizeof(char *), GFP_KERNEL);
	dma_addrs = kcalloc(completions, sizeof(dma_addr_t), GFP_KERNEL);

	if (!inputs || !outputs || !dma_addrs) {
		pr_err("%s inputs=%pS outputs=%pS dma=%pS\n", __func__, inputs, outputs, dma_addrs);
		return -ENOMEM;
	}
	

	for (i = 0; i < completions; i++) {
		inputs[i] = kzalloc(4096, GFP_KERNEL);
		if (!inputs[i])
			return -ENOMEM;
	}

	for (i = 0; i < completions; i++) {
		outputs[i] = kzalloc(4096, GFP_KERNEL);
		if (!outputs[i])
			return -ENOMEM;
	}

	while (time_before(jiffies, stop)) {
		for (i = 0; i < completions; i++) {
			dma_addrs[i] = test_mapsingle(dev, inputs[i], 4096);
			test_memcpy(outputs[i], inputs[i], 4096);
		}
		msleep(10+cpu%5);
		for (i = 0; i < completions; i++) {
			test_unmapsingle(dev, inputs[i], 4096, dma_addrs[i]);
		}
		mappings[cpu] += completions;
	}

	for (i = 0; i < completions; i++) {
		kfree(outputs[i]);
		kfree(inputs[i]);
	}

	kfree(inputs);
	kfree(outputs);
	kfree(dma_addrs);

	up(sem);

	return 0;
}  

static int timerthread(void *data)
{  
	unsigned long stop = jiffies +seconds*HZ;
	struct semaphore *sem = data;
	unsigned long long previous_mappings = 0;
	int i;

	while (time_before(jiffies, stop)) {
		unsigned long long _mappings = 0;
		msleep(30000);
		for(i=0;i<ways;i++) {
			_mappings += mappings[i];
		}
		pr_err("%s mappings=%llu\n", __func__, _mappings - previous_mappings);
		previous_mappings = _mappings;
	}

	up(sem);

	return 0;
}  


int smmu_test;


extern ktime_t arm_smmu_cmdq_get_average_time(void);
extern void arm_smmu_cmdq_zero_times(void);
extern void arm_smmu_cmdq_zero_cmpxchg(void);
extern u64 arm_smmu_cmdq_get_tries(void);
extern u64 arm_smmu_cmdq_get_cmpxcgh_fails(void);


void smmu_test_core(int cpus)
{
	struct task_struct *tsk;
	int i;
	unsigned long long total_mappings = 0;
	smmu_test = 1;

	ways = cpus;
	arm_smmu_cmdq_zero_times();
	arm_smmu_cmdq_zero_cmpxchg();

	if (ways > 200) {
		seconds = (ways - 200) * 60;
		pr_err("setting seconds to %d (%d minutes)\n", seconds, seconds/60);
	}
	if (ways > num_possible_cpus()) {
		ways = num_possible_cpus();
		pr_err("limiting ways to %d\n", ways);
	}

	if (completions > COMPLETIONS_SIZE) {
		completions = COMPLETIONS_SIZE;
		pr_err("limiting completions to %d\n", completions);
	}

	for(i=0;i<ways;i++) {
		mappings[i] = 0;
		tsk = kthread_create_on_cpu(testthread, &sem[i], i,  "map_test");

		if (IS_ERR(tsk))
			printk(KERN_ERR "create test thread failed i=%d\n", i);
		wake_up_process(tsk);
	}

	if (seconds > TIMER_PERIOD_SECONDS * 2) {
		tsk = kthread_create(timerthread, &sem[NR_CPUS], "map_timer");
		if (IS_ERR(tsk))
			printk(KERN_ERR "timer test thread failed\n");
		wake_up_process(tsk);
	}

	for(i=0;i<ways;i++) {
		down(&sem[i]);
		total_mappings += mappings[i];
	}
	smmu_test = 0;

	printk(KERN_ERR "finished total_mappings=%llu (per way=%llu) (rate=%llu per second per cpu) ways=%d average=%lld tries=%lld cmpxcgh tries=%lld\n", 
	total_mappings, total_mappings / ways, total_mappings / (seconds* ways), ways,
	arm_smmu_cmdq_get_average_time(),
	arm_smmu_cmdq_get_tries(),
	arm_smmu_cmdq_get_cmpxcgh_fails());

}
EXPORT_SYMBOL(smmu_test_core);


static int __init test_init(void)
{
	int i;

	for(i=0;i<=NR_CPUS;i++)
		sema_init(&sem[i], 0);

	return 0;
}
  
static void __exit test_exit(void)
{
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
