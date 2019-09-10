// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon SPI Nor Flash Controller Driver
 *
 * Copyright (c) 2015-2016 HiSilicon Technologies Co., Ltd.
 */
#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#define HIFMC_DMA_MAX_LEN		(4096)

#define VERSION (0x1f8)

#define CMD_CONFIG (0x300)
#define CMD_CONFIG_DATA_CNT_OFF 9
#define CMD_CONFIG_DATA_CNT_MSK (0xff << CMD_CONFIG_DATA_CNT_OFF)
#define CMD_CONFIG_CMD_RW_OFF 8
#define CMD_CONFIG_CMD_RW_MSK (1 << CMD_CONFIG_CMD_RW_OFF)
#define CMD_CONFIG_CMD_DATA_EN_OFF 7
#define CMD_CONFIG_CMD_DATA_EN_MSK (1 << CMD_CONFIG_CMD_DATA_EN_OFF)
#define CMD_CONFIG_CMD_DUMMY_CNT_OFF 4
#define CMD_CONFIG_CMD_DUMMY_CNT_MSK (0x7 << CMD_CONFIG_CMD_DUMMY_CNT_OFF)
#define CMD_CONFIG_CMD_ADDR_EN_OFF 3
#define CMD_CONFIG_CMD_ADDR_EN_MSK (1 << CMD_CONFIG_CMD_ADDR_EN_OFF)
#define CMD_CONFIG_CMD_CS_SEL_OFF 1
#define CMD_CONFIG_CMD_CS_SEL_MSK (1 << CMD_CONFIG_CMD_CS_SEL_OFF)
#define CMD_CONFIG_CMD_START_OFF 0
#define CMD_CONFIG_CMD_START_MSK (1 << CMD_CONFIG_CMD_START_OFF)
#define CMD_INS (0x308)
#define CMD_ADDR (0x30c)
#define BUS_FLASH_SIZE (0x210)
#define BUS_CFG1 (0x200)
#define BUS_CFG2 (0x204)
#define CMD_DATABUF(x) (0x400 + (x * 4))



enum hifmc_iftype {
	IF_TYPE_STD,
	IF_TYPE_DUAL,
	IF_TYPE_DIO,
	IF_TYPE_QUAD,
	IF_TYPE_QIO,
};

struct hifmc_priv {
	u32 chipselect;
//	u32 clkrate;
	struct hifmc_host *host;
};

#define HIFMC_MAX_CHIP_NUM		2
struct hifmc_host {
	struct device *dev;
	struct mutex lock;

	void __iomem *regbase;
	void __iomem *iobase;
//	struct clk *clk;
//	void *buffer;
	dma_addr_t dma_buffer;

	u32 num_chip;
};

static inline int hisi_spi_hi16xx_spi_wait_op_finish(struct hifmc_host *host)
{
	pr_err("%s host=%pS\n", __func__, host);
	return 0;
}

static void hisi_spi_hi16xx_spi_init(struct hifmc_host *host)
{
	pr_err("%s host=%pS\n", __func__, host);
}

static int hisi_spi_hi16xx_spi_read_reg(struct hifmc_host *host, u8 opcode, u8 *buf,
		int len, int chip_select)
{
	u32 config, version;
	__le32 cmd_buf0, cmd_buf1, cmd_buf2, cmd_buf3,  cmd_buf[5], bus_cfg1, bus_cfg2;
	int i;
	int count = 0;
	int bus_flash_size;

	config = readl(host->regbase + CMD_CONFIG);
	version = readl(host->regbase + VERSION);
	bus_cfg1 = readl(host->regbase + BUS_CFG1);
	bus_cfg2 = readl(host->regbase + BUS_CFG2);
	bus_flash_size = readl(host->regbase + BUS_FLASH_SIZE);
	cmd_buf0 = readl(host->regbase + CMD_DATABUF(0));
	cmd_buf1 = readl(host->regbase + CMD_DATABUF(1));

	pr_err("%s opcode=0x%x buf=%pS len=%d count=%d chip_select=%d config=0x%x bus_cfg1=0x%x bus_cfg2=0x%x\n",
		__func__, opcode, buf, len, count, chip_select, config, bus_cfg1, bus_cfg2);
//	pr_err("%s bus_flash_size=0x%x\n", __func__, bus_flash_size);

//	pr_err("%s1 config=0x%x ins=0x%x addr=0x%x version=0x%x cmd_buf0=0x%x cmd_buf1=0x%x\n",
//		__func__, config, ins, addr, version, cmd_buf0, cmd_buf1);

	config &= ~CMD_CONFIG_DATA_CNT_MSK & ~CMD_CONFIG_CMD_CS_SEL_MSK &
			~CMD_CONFIG_CMD_ADDR_EN_MSK & ~CMD_CONFIG_CMD_RW_MSK;
	config = 0;
	config |= ((len +1 )<< CMD_CONFIG_DATA_CNT_OFF) | CMD_CONFIG_CMD_DATA_EN_MSK |
			CMD_CONFIG_CMD_START_MSK | chip_select << CMD_CONFIG_CMD_CS_SEL_OFF
			 | CMD_CONFIG_CMD_RW_MSK;

	writel(opcode, host->regbase + CMD_INS);

//	pr_err("%s2 config=0x%x ins=0x%x addr=0x%x version=0x%x cmd_buf0=0x%x\n",
//		__func__, config, ins, addr, version, cmd_buf0);

	writel(config, host->regbase + CMD_CONFIG);
sleep:
	count++;
	config = readl(host->regbase + CMD_CONFIG);
	if (config & CMD_CONFIG_CMD_START_MSK)
		goto sleep;
//	pr_err("%s3 config=0x%x ins=0x%x addr=0x%x version=0x%x cmd_buf0=0x%x\n",
//		__func__, config, ins, addr, version, cmd_buf0);

	cmd_buf0 = readl(host->regbase + CMD_DATABUF(0));
	cmd_buf1 = readl(host->regbase + CMD_DATABUF(1));
	cmd_buf2 = readl(host->regbase + CMD_DATABUF(2));
	cmd_buf3 = readl(host->regbase + CMD_DATABUF(3));
	cmd_buf[0] = le32_to_cpu(cmd_buf0);
	cmd_buf[1] = le32_to_cpu(cmd_buf1);
	cmd_buf[2] = le32_to_cpu(cmd_buf2);
	cmd_buf[3] = le32_to_cpu(cmd_buf3);

//	pr_err("%s4 config=0x%x opcode=0x%x version=0x%x cmd_buf0=0x%x cmd_buf[0]=0x%x  cmd_buf[1]=0x%x count=%d\n",
//		__func__, config, opcode, version, cmd_buf0, cmd_buf[0 ], cmd_buf[1], count);

	for (i = 0; i<len;i++) {
		u8 *byte = (u8 *)&cmd_buf[0];

		//pr_err("%s byte %d=0x%x", __func__, i, byte[i]);
		*buf = byte[i];
		buf++;
	}

	count++;

	return 0;
}



static int hisi_spi_hi16xx_spi_write_reg(struct hifmc_host *host, u8 opcode, u8 *buf,
		int len, int chip_select)
{
	pr_err("%s opcode=0x%x buf=%pS len=%d chip_select=%d\n",
		__func__, opcode, buf, len, chip_select);


	return -1;
}


#define MAX_CMD_WORD 4

static ssize_t hisi_spi_hi16xx_spi_read(struct hifmc_host *host, loff_t from, size_t len,
		u_char *read_buf, int read_opcode, int read_dummy, int chip_select)
{
	u32 config, ins, addr, version, cmd_buf0, cmd_buf1;
	int i;
	int count = 0;
	int remaining = len;
	//WARN_ON_ONCE(1);
	config = readl(host->regbase + CMD_CONFIG);
	ins = readl(host->regbase + CMD_INS);
	addr = readl(host->regbase + CMD_ADDR);
	version = readl(host->regbase + VERSION);
	cmd_buf0 = readl(host->regbase + CMD_DATABUF(0));
	cmd_buf1 = readl(host->regbase + CMD_DATABUF(1));
	
	
//	pr_err_ratelimited("%s read_buf=%pS len=%ld host=%pS count=%d read opcode=0x%x addr=0x%x chip_select=%d\n", __func__, 
//		read_buf, len, host, count, read_opcode, addr, chip_select);
	//	pr_err("%s1 spi=%pS config=0x%x ins=0x%x addr=0x%x version=0x%x cmd_buf0=0x%x cmd_buf1=0x%x\n",
	//		__func__, spi, config, ins, addr, version, cmd_buf0, cmd_buf1);



	do {
		int read_len;

		if (remaining > MAX_CMD_WORD * 4)
			read_len = MAX_CMD_WORD * 4;
		else
			read_len = remaining;

		remaining -= read_len;

		config &= ~CMD_CONFIG_DATA_CNT_MSK & ~CMD_CONFIG_CMD_CS_SEL_MSK &
				~CMD_CONFIG_CMD_DATA_EN_OFF;
		config |= ((read_len + 1) << CMD_CONFIG_DATA_CNT_OFF) | CMD_CONFIG_CMD_DATA_EN_MSK |
			    CMD_CONFIG_CMD_ADDR_EN_MSK |
				(read_dummy / 8) << CMD_CONFIG_CMD_DUMMY_CNT_OFF |
				CMD_CONFIG_CMD_START_MSK | CMD_CONFIG_CMD_RW_MSK;// 1: READ
		writel(from, host->regbase + CMD_ADDR);
		writel(read_opcode, host->regbase + CMD_INS);
		writel(config, host->regbase + CMD_CONFIG);

		from += read_len;

//		pr_err("%s1 read_buf=%pS len=%ld host=%pS count=%d config=0x%x read_len=%x remaining=%d\n", __func__, read_buf, len, host, count, config, read_len, remaining);

sleep:
		count++;
		config = readl(host->regbase + CMD_CONFIG);
		addr = readl(host->regbase + CMD_ADDR);

		if (config & CMD_CONFIG_CMD_START_MSK)
			goto sleep;

//		pr_err("%s2 read_buf=%pS len=%ld host=%pS count=%d config=0x%x addr=0x%x\n", __func__, read_buf, len, host, count, config, addr);

		for (i=0;i<(read_len + MAX_CMD_WORD - 1)/MAX_CMD_WORD;i++) {
			u32 cmd_bufx = readl(host->regbase + CMD_DATABUF(i));
	//		u32 cmd_bufy = __swab32(cmd_bufx);
			u8 *ptr = (u8 *)&cmd_bufx;
			u8 aa, bb, cc, dd;

	//		pr_err("%s3.0 i=%d cmd_bufx=0x%x cmd_bufy=0x%x\n", __func__, i, cmd_bufx, cmd_bufy);

			*read_buf = aa = ptr[0];read_buf++;
			*read_buf = bb = ptr[1];read_buf++;
			*read_buf = cc = ptr[2];read_buf++;
			*read_buf = dd = ptr[3];read_buf++;
			
	//		pr_err("%s3.1 i=%d cmd_bufx=0x%x [%02x %02x %02x %02x] remaining=%d count=%d\n", 
	//			__func__, i, cmd_bufx, aa, bb, cc, dd, remaining, count);
		}
	}while (remaining);
//	pr_err("%s out returning len=%ld\n", __func__, len);
	return len;
}


static ssize_t hisi_spi_hi16xx_spi_write(struct hifmc_host *host, loff_t from, size_t len,
		u_char *write_buf, int write_opcode, int write_dummy, int chip_select)
{
	
	pr_err("%s write_buf=%pS len=%ld read write_opcode=0x%x chip_select=%d from=0x%llx\n", __func__, 
		write_buf, len, write_opcode, chip_select, from);

	return -1;
}


__maybe_unused static int hi16xx_spi_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
//	struct hifmc_host *host = spi_controller_get_devdata(mem->spi->master);

	//pr_err("%s mem=%pS op=%pS host=%pS\n", __func__, mem, op, host);

	return 0;
}

__maybe_unused static bool hi16xx_spi_supports_op(struct spi_mem *mem,
				 const struct spi_mem_op *op)
{
//	struct hifmc_host *host = spi_controller_get_devdata(mem->spi->master);

	//pr_err("%s mem=%pS op=%pS [] host=%pS\n", __func__, mem, op, host);

	return true;
}

static int hi16xx_spi_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct hifmc_host *host = spi_controller_get_devdata(mem->spi->master);
	struct spi_device *spi = mem->spi;
	int chip_select = spi->chip_select;

//	pr_err("%s mem=%pS [cmd opcode=0x%x buswidth=0x%x] host=%pS\n", __func__, mem, 
//		op->cmd.opcode, op->cmd.buswidth, host);
//	pr_err("%s1 mem=%pS [addr nbytes=0x%x buswidth=0x%x val=0x%llx] host=%pS\n", __func__, mem, 
//		op->addr.nbytes, op->addr.buswidth, op->addr.val, host);
//	pr_err("%s2 mem=%pS [dummy buswidth=0x%x buswidth=0x%x] host=%pS\n", __func__, mem, 
//		op->dummy.nbytes, op->dummy.buswidth, host);
//	pr_err("%s3 mem=%pS [data nbytes=0x%x buf in=0x%pS out=%pS dir=%d] host=%pS\n", __func__, mem, 
//		op->data.nbytes, op->data.buf.in, op->data.buf.out, op->data.dir, host);

	if (op->addr.nbytes == 0 && op->addr.buswidth == 0 && op->addr.val == 0) {
		pr_err("%s [cmd opcode=0x%x buswidth=0x%x, addr.nbytes=%d buswidth=%d val=0x%llx, dummy nbytes=%d buswidth=%d, data.buswidth=%d buf=%pS]\n", __func__,  
		op->cmd.opcode, op->cmd.buswidth, op->addr.nbytes, op->addr.buswidth, op->addr.val, op->dummy.nbytes, op->dummy.buswidth, op->data.buswidth, op->data.buf.in);
		/* Read/write Reg */
		switch (op->data.dir) {
		case SPI_MEM_DATA_IN:
			return hisi_spi_hi16xx_spi_read_reg(host, op->cmd.opcode, op->data.buf.in, op->data.nbytes, chip_select);
		case SPI_MEM_DATA_OUT:
			return hisi_spi_hi16xx_spi_write_reg(host, op->cmd.opcode, op->data.buf.in, op->data.nbytes, chip_select);
		default:
			break;
		}
	} else {
		/* Read/write */
		switch (op->data.dir) {
		case SPI_MEM_DATA_IN:
			return hisi_spi_hi16xx_spi_read(host, op->addr.val, op->data.nbytes, op->data.buf.in, op->cmd.opcode, op->dummy.nbytes*8, chip_select);
		case SPI_MEM_DATA_OUT:
			return hisi_spi_hi16xx_spi_write(host, op->addr.val, op->data.nbytes, op->data.buf.in, op->cmd.opcode, op->dummy.nbytes*8, chip_select);
		default:
			break;
		}
	}
	
	pr_err("%s not support [cmd opcode=0x%x buswidth=0x%x addr.val=0x%llx]\n", __func__, 
		op->cmd.opcode, op->cmd.buswidth, op->addr.val);
	return -ENOTSUPP;
}
static const char *hi16xx_spi_get_name(struct spi_mem *mem)
{
	struct hifmc_host *host = spi_controller_get_devdata(mem->spi->master);

	pr_err("%s mem=%pS host=%pS\n", __func__, mem, host);

	return "snake";
}


static const struct spi_controller_mem_ops hi16xx_spi_mem_ops = {
//	.adjust_op_size = hi16xx_spi_adjust_op_size,
//	.supports_op = hi16xx_spi_supports_op,
	.exec_op = hi16xx_spi_exec_op,
	.get_name = hi16xx_spi_get_name,
};

void alloc_fake_spi_chip(struct device *host, struct spi_controller *ctlr, int cs)
{
	
	struct spi_device *spi = spi_alloc_device(ctlr);
	struct device *spi_dev;
	const char *compatible = "jedec,spi-nor", *p;
	dev_err(host, "%s creating fake SPI NOR device cs=%d\n", __func__, cs);
	if (!spi) {
		dev_err(host, "failed to allocate SPI device for \n");
	}
	//	int cplen;
	
	//	compatible = of_get_property(node, "compatible", &cplen);
	//	if (!compatible || strlen(compatible) > cplen)
	//		return -ENODEV;
	p = strchr(compatible, ',');
	strlcpy(spi->modalias, p ? p + 1 : compatible, sizeof(spi->modalias));
	
	spi_dev = &spi->dev;
	
	spi_dev->parent = host;
	spi->max_speed_hz	= 48000000;
	
	spi->chip_select = cs;
	
	if (spi_add_device(spi)) {
		dev_err(host, "failed to add SPI device from ACPI\n");
		spi_dev_put(spi);
	}

}

static int hisi_spi_hi16xx_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct hifmc_host *host;
	struct spi_controller *ctlr;
	struct device_node *np = dev->of_node;
	int version;
	int ret;

	dev_err(dev, "%s np=%pS\n", __func__, np);

	ctlr = spi_alloc_master(&pdev->dev, sizeof(*host));
	if (!ctlr)
		return -ENOMEM;
	
	ctlr->mode_bits = SPI_RX_DUAL | SPI_RX_QUAD |
			  SPI_TX_DUAL | SPI_TX_QUAD;

	dev_err(dev, "%s2\n", __func__);

	host = spi_controller_get_devdata(ctlr);
	host->dev = dev;
//	host->devtype_data = of_device_get_match_data(dev);
//	if (!host->devtype_data) {
//		ret = -ENODEV;
//		goto err_put_ctrl;
//	}

	platform_set_drvdata(pdev, host);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg");
	host->regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->regbase))
		return PTR_ERR(host->regbase);
	version = readl(host->regbase + VERSION);
	dev_err(dev, "%s host->regbase=%p\n", __func__, host->regbase);
	dev_err(dev, "%s version=0x%x\n", __func__, version);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "memory");
	host->iobase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->iobase))
		return PTR_ERR(host->iobase);


	#ifdef hos_buffer
	host->buffer = dmam_alloc_coherent(dev, HIFMC_DMA_MAX_LEN,
			&host->dma_buffer, GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;
	#endif

	mutex_init(&host->lock);
	hisi_spi_hi16xx_spi_init(host);
	ctlr->dev.of_node = np;

	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mem_ops = &hi16xx_spi_mem_ops;

	ret = devm_spi_register_controller(dev, ctlr);
	dev_err(dev, "%s devm_spi_register_controller ret=%d\n", __func__, ret);


	if (!acpi_disabled) {
		int chip_sel;
		for (chip_sel=0;chip_sel<ctlr->num_chipselect;chip_sel++)
			alloc_fake_spi_chip(dev, ctlr, chip_sel);
	}

	return ret;
}

static int hisi_spi_hi16xx_spi_remove(struct platform_device *pdev)
{
	struct hifmc_host *host = platform_get_drvdata(pdev);

//	hisi_spi_hi16xx_spi_unregister_all(host);
	mutex_destroy(&host->lock);
	return 0;
}

static const struct of_device_id hisi_spi_hi16xx_spi_dt_ids[] = {
	{ .compatible = "hisilicon,sfc-hi16xx"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hisi_spi_hi16xx_spi_dt_ids);

static const struct acpi_device_id hisi_spi_hi16xx_spi_acpi_ids[] = {
	{"HISI0999", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, hisi_spi_hi16xx_spi_acpi_ids);

#define HI16XX_SFC_NAME "hisi-sfc-hi16xx"




static const struct platform_device_id hi16xx_sfc_match[] = {
	{ HI16XX_SFC_NAME, 0},
	{ }
};
MODULE_DEVICE_TABLE(platform, hi16xx_sfc_match);


static struct platform_driver hisi_spi_hi16xx_spi_driver = {
	.id_table = hi16xx_sfc_match,
	.driver = {
		.name	= HI16XX_SFC_NAME,
		.of_match_table = hisi_spi_hi16xx_spi_dt_ids,
		.acpi_match_table = ACPI_PTR(hisi_spi_hi16xx_spi_acpi_ids),
	},
	.probe	= hisi_spi_hi16xx_spi_probe,
	.remove	= hisi_spi_hi16xx_spi_remove,
};
//module_platform_driver(hisi_spi_hi16xx_spi_driver);

static struct resource hi16xx_sfc_hi1616_resources[] = {
	{
		/* irq */
		.flags          = IORESOURCE_MEM,
		.name = "reg",
		.start = 0xa6000000,
		.end = 0xa6000000 + 0x10000 -1,
	},
	{
		/* irq */
		.flags          = IORESOURCE_MEM,
		.name = "memory",
		.start = 0xa4000000,
		.end = 0xa4000000 + 0x10000 -1,
	},
};


static struct platform_device hi1616_spi_dev = {
	.name = HI16XX_SFC_NAME,
	.id = -1,
	.resource = hi16xx_sfc_hi1616_resources,
	.num_resources = ARRAY_SIZE(hi16xx_sfc_hi1616_resources)
};
	
static struct resource hi16xx_sfc_hi1620_resources[] = {
	{
		/* irq */
		.flags          = IORESOURCE_MEM,
		.name = "reg",
		.start = 0x206200000,
		.end = 0x206200000 + 0x10000 -1,
	},
	{
		/* irq */
		.flags          = IORESOURCE_MEM,
		.name = "memory",
		.start = 0x206250000,
		.end = 0x206250000 + 0x10000 -1,
	},
};


static struct platform_device hi1620_spi_dev = {
	.name = HI16XX_SFC_NAME,
	.id = -1,
	.resource = hi16xx_sfc_hi1620_resources,
	.num_resources = ARRAY_SIZE(hi16xx_sfc_hi1620_resources)
};

#define read_cpuid(reg)			read_sysreg_s(SYS_ ## reg)

static int __init hisi_spi_hi16xx_spi_module_init(void)
{
	int ret;
	u32 midr = read_cpuid_id();

	pr_err("%s acpi_disabled=%d midr=0x%x\n", __func__, acpi_disabled, midr);

	ret = platform_driver_register(&hisi_spi_hi16xx_spi_driver);
	if (ret)
		return ret;

	if (acpi_disabled)
		return 0;

	switch (midr) {
	case 0x410fd082:
	ret = platform_device_register(&hi1616_spi_dev);
	if (ret) {
		pr_err("%s could not register hi1616_spi_dev pdev ret=%d\n", __func__, ret);
		return ret;
	}
	break;
	case 0x480fd010:
	ret = platform_device_register(&hi1620_spi_dev);
	if (ret) {
		pr_err("%s could not register hi1620_spi_dev pdev ret=%d\n", __func__, ret);
		return ret;
	}
	break;
	default:
		break;
	}
	
	pr_err("%s registered pdev\n", __func__);
	
	return 0;	
}
module_init(hisi_spi_hi16xx_spi_module_init);

static void __exit hisi_spi_hi16xx_spi_module_remove(void)
{

}
module_exit(hisi_spi_hi16xx_spi_module_remove);



MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HiSilicon SPI spi Flash Controller Driver");
