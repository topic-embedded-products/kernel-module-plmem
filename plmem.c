// SPDX-License-Identifier: GPL-2.0

#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define DRIVER_CLASS_NAME "plmem"

#define NUMBER_OF_CDEV 1

enum plmem_mode { noncached, writecombine, cached };

struct plmem_data {
	struct device *dev;
	resource_size_t mem_start; /* For mmap */
	unsigned long mem_size;
	dev_t devt;
	struct cdev cdev_command;
	enum plmem_mode mmap_mode;
};

static struct class *plmem_class;

static int plmem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct plmem_data *data = filp->private_data;

	switch (data->mmap_mode) {
	case noncached:
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		break;
	case writecombine:
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		break;
	default:
		break;
	}
	return vm_iomap_memory(vma, data->mem_start, data->mem_size);
}

static long plmem_ioctl(
	struct file *filp, unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

static int plmem_open(struct inode *inode, struct file *filp)
{
	struct plmem_data *data =
		container_of(inode->i_cdev, struct plmem_data, cdev_command);

	filp->private_data = data; /* for other methods */

	return 0;
}

static int plmem_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations plmem_fops = {
	.owner = THIS_MODULE,
	.mmap = plmem_mmap,
	.unlocked_ioctl = plmem_ioctl,
	.open = plmem_open,
	.release = plmem_release,
};

static int plmem_create_cdev(struct plmem_data *data, const char *label)
{
	int ret;
	dev_t devt;
	struct device *device = data->dev;
	struct device *char_device;

	ret = alloc_chrdev_region(&devt, 0, NUMBER_OF_CDEV, DRIVER_CLASS_NAME);
	if (ret < 0)
		return ret;

	data->devt = devt;

	cdev_init(&data->cdev_command, &plmem_fops);
	data->cdev_command.owner = THIS_MODULE;
	ret = cdev_add(&data->cdev_command, devt, 1);
	if (ret) {
		dev_err(device, "cdev_add() failed\n");
		goto failed_cdev;
	}

	char_device = device_create(plmem_class, device, devt, data, label);
	if (IS_ERR(char_device)) {
		dev_err(device, "unable to create device %s\n", label);
		ret = PTR_ERR(char_device);
		goto failed_device_create;
	}

	return 0;

failed_device_create:
failed_cdev:
	unregister_chrdev_region(devt, NUMBER_OF_CDEV);
	return ret;
}

static int plmem_probe(struct platform_device *pdev)
{
	struct plmem_data *data;
	struct resource *res;
	const char *label = "plmem";
	const char *mem_type;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	platform_set_drvdata(pdev, data);
	data->dev = &pdev->dev;

	/* Request I/O resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	data->mem_start = res->start;
	data->mem_size = res->end - res->start + 1;
	if (device_is_compatible(&pdev->dev, "topic,iotester"))
		data->mmap_mode = noncached;
	else
		data->mmap_mode = writecombine; // default

	ret = device_property_read_string(&pdev->dev, "topic,mem-type", &mem_type);
	if (!ret) {
		if (!strcmp(mem_type, "writecombine"))
			data->mmap_mode = writecombine;
		else if (!strcmp(mem_type, "cached"))
			data->mmap_mode = cached;
		else if (!strcmp(mem_type, "noncached"))
			data->mmap_mode = noncached;
		else
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "Invalid mem-type: %s\n", mem_type);
	}

	ret = device_property_read_string(&pdev->dev, "label", &label);
	if (ret)
		dev_err(&pdev->dev, "No label, using default: %s\n", label);

	ret = plmem_create_cdev(data, label);

	return ret;
}

static void plmem_remove(struct platform_device *pdev)
{
	struct plmem_data *data = dev_get_drvdata(&pdev->dev);
	int i;

	for (i = 0; i < NUMBER_OF_CDEV; ++i)
		device_destroy(plmem_class, data->devt + i);
	unregister_chrdev_region(data->devt, data->devt + NUMBER_OF_CDEV - 1);
}

static struct of_device_id plmem_match[] = {
	{ .compatible = "topic,plmem", },
	{ .compatible = "topic,iotester", },
	{},
};
MODULE_DEVICE_TABLE(of, plmem_match);

static struct platform_driver plmem_driver = {
	.probe  = plmem_probe,
	.remove = plmem_remove,
	.driver = {
		.name = "plmem",
		.of_match_table = plmem_match,
	}
};

static int __init plmem_init(void)
{
	plmem_class = class_create(DRIVER_CLASS_NAME);
	if (IS_ERR(plmem_class)) {
		pr_err("Unable to create plmem class; errno = %ld\n",
			PTR_ERR(plmem_class));
		return PTR_ERR(plmem_class);
	}

	return platform_driver_register(&plmem_driver);
}

static void __exit plmem_exit(void)
{
	platform_driver_unregister(&plmem_driver);
	class_destroy(plmem_class);
}

module_init(plmem_init);
module_exit(plmem_exit);

MODULE_AUTHOR("Mike Looijmans <mike.looijmans@topic.nl>");
MODULE_DESCRIPTION("Driver for FPGA memory tests");
MODULE_LICENSE("GPL v2");
