/*
 * drivers/gpu/ion/sunxi/sunxi_ion.c
 *
 * Copyright(c) 2013-2015 Allwinnertech Co., Ltd.
 *      http://www.allwinnertech.com
 *
 * Author: liugang <liugang@allwinnertech.com>
 *
 * sunxi ion heap realization
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/plist.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/anon_inodes.h>
#include <linux/ion.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/miscdevice.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/ion_sunxi.h>
#include <linux/vmalloc.h>
#include <asm/setup.h>
#include <mach/system.h>
#include "../ion_priv.h"

#define DEV_NAME	"ion-sunxi"

struct ion_device;
struct ion_heap **pheap;
struct ion_heap *carveout_heap = NULL;
int num_heaps;
extern struct tag_mem32 ion_mem;
long sunxi_ion_ioctl(struct ion_client *client, unsigned int cmd, unsigned long arg);

int flush_clean_user_range(long start, long end);
EXPORT_SYMBOL(flush_clean_user_range);

int flush_user_range(long start, long end);
EXPORT_SYMBOL(flush_user_range);

void flush_dcache_all(void);
EXPORT_SYMBOL(flush_dcache_all);

/*
 * use sunxi_map_kernel to map phys addr to kernel space, instead of ioremap,
 * which cannot be used for mem_reserve areas.
 */
void *sunxi_map_kernel(unsigned int phys_addr, unsigned int size)
{
	int npages = PAGE_ALIGN(size) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	struct page *cur_page = phys_to_page(phys_addr);
	pgprot_t pgprot;
	void *vaddr;
	int i;

	if(!pages)
		return 0;

	for(i = 0; i < npages; i++)
		*(tmp++) = cur_page++;

	pgprot = pgprot_noncached(PAGE_KERNEL);
	vaddr = vmap(pages, npages, VM_MAP, pgprot);

	vfree(pages);
	return vaddr;
}
EXPORT_SYMBOL(sunxi_map_kernel);

void sunxi_unmap_kernel(void *vaddr)
{
	vunmap(vaddr);
}
EXPORT_SYMBOL(sunxi_unmap_kernel);

unsigned int sunxi_mem_alloc(unsigned int size)
{
	ion_phys_addr_t phys_addr;

	if(unlikely(!carveout_heap))
		return 0;
	phys_addr = ion_carveout_allocate(carveout_heap, size, 0);
	if((ion_phys_addr_t)ION_CARVEOUT_ALLOCATE_FAIL == phys_addr)
		return 0;
	return phys_addr;
}
EXPORT_SYMBOL_GPL(sunxi_mem_alloc);

void sunxi_mem_free(unsigned int phys_addr, unsigned int size)
{
	ion_carveout_free(carveout_heap, phys_addr, size);
}
EXPORT_SYMBOL_GPL(sunxi_mem_free);

int sunxi_ion_probe(struct platform_device *pdev)
{
	struct ion_platform_data *pdata = pdev->dev.platform_data;
	struct ion_platform_heap *heaps_desc;
	struct ion_device *idev;
	int i, ret = 0;

	pheap = kzalloc(sizeof(struct ion_heap *) * pdata->nr, GFP_KERNEL);
	idev = ion_device_create(sunxi_ion_ioctl);
	if(IS_ERR_OR_NULL(idev)) {
		kfree(pheap);
		return PTR_ERR(idev);
	}

	for(i = 0; i < pdata->nr; i++) {
		heaps_desc = &pdata->heaps[i];
		if(heaps_desc->type == ION_HEAP_TYPE_CARVEOUT) {
			heaps_desc->base = ION_CARVEOUT_MEM_BASE;
			heaps_desc->size = sun7i_ion_carveout_size();
		}
		pheap[i] = ion_heap_create(heaps_desc);
		if(IS_ERR_OR_NULL(pheap[i])) {
			ret = PTR_ERR(pheap[i]);
			goto err;
		}
		ion_device_add_heap(idev, pheap[i]);

		if(heaps_desc->type == ION_HEAP_TYPE_CARVEOUT)
			carveout_heap = pheap[i];
	}

	num_heaps = i;
	platform_set_drvdata(pdev, idev);
	return 0;
err:
	while(i--)
		ion_heap_destroy(pheap[i]);
	ion_device_destroy(idev);
	kfree(pheap);
	return ret;
}

int sunxi_ion_remove(struct platform_device *pdev)
{
	struct ion_device *idev = platform_get_drvdata(pdev);

	while(num_heaps--)
		ion_heap_destroy(pheap[num_heaps]);
	kfree(pheap);
	ion_device_destroy(idev);
	return 0;
}

static struct ion_platform_data ion_data = {
	.nr = 3,
	.heaps = {
		[0] = {
			.type = ION_HEAP_TYPE_SYSTEM,
			.id = (u32)ION_HEAP_TYPE_SYSTEM,
			.name = "sytem",
		},
		[1] = {
			.type = ION_HEAP_TYPE_SYSTEM_CONTIG,
			.id = (u32)ION_HEAP_TYPE_SYSTEM_CONTIG,
			.name = "system_contig",
		},
		[2] = {
			.type = ION_HEAP_TYPE_CARVEOUT,
			.id = (u32)ION_HEAP_TYPE_CARVEOUT,
			.name = "carveout",
			.base = ION_CARVEOUT_MEM_BASE, 
			.size = ION_CARVEOUT_MEM_SIZE_DEFAULT,
			.align = 0,
			.priv = NULL,
		},
	}
};


static struct platform_device ion_dev = {
	.name = DEV_NAME,
	.dev = {
		.platform_data = &ion_data,
	}
};
static struct platform_driver ion_driver = {
	.probe = sunxi_ion_probe,
	.remove = sunxi_ion_remove,
	.driver = {.name = DEV_NAME}
};

static int __init sunxi_ion_init(void)
{
	int ret;

	ret = platform_device_register(&ion_dev);
	if(ret)
		return ret;
	return platform_driver_register(&ion_driver);
}

static void __exit sunxi_ion_exit(void)
{
	platform_driver_unregister(&ion_driver);
	platform_device_unregister(&ion_dev);
}

subsys_initcall(sunxi_ion_init);
module_exit(sunxi_ion_exit);
