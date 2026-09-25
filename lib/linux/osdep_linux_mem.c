// SPDX-License-Identifier: LGPL
/* Copyright (C) 2018-2019 Jiaxun Yang <jiaxun.yang@flygoat.com> */
/* Access PCI Config Space - libpci */
#include <sys/mman.h>
#include <pci/pci.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "osdep_linux_mem.h"

/*
 * State of the /dev/mem window onto the PM table.
 *
 * These used to be a single non-static `void *phy_map`, which (a) was an
 * exported symbol of libryzenadj and (b) silently leaked the previous mapping
 * whenever init_mem_obj_mem() ran twice (init_table() can be re-entered
 * through the lazy-init path). We now keep the base, the mapped length and
 * the offset of the table inside the page, and unmap before remapping.
 */
static void *phy_map = MAP_FAILED;   /* page-aligned mapping base */
static size_t phy_map_len;           /* bytes actually mapped */
static size_t phy_map_offset;        /* offset of the table within the mapping */

static size_t page_size(void)
{
	const long sz = sysconf(_SC_PAGESIZE);

	return sz > 0 ? (size_t)sz : 4096u;
}

static void unmap_phy(void)
{
	if (phy_map != MAP_FAILED) {
		munmap(phy_map, phy_map_len);
		phy_map = MAP_FAILED;
	}

	phy_map_len = 0;
	phy_map_offset = 0;
}

/* Pointer to the start of the PM table inside the mapping, or NULL. */
static const void *phy_table_ptr(void)
{
	if (phy_map == MAP_FAILED)
		return NULL;

	return (const unsigned char *)phy_map + phy_map_offset;
}

/* How many bytes of table are actually readable through the mapping. */
static size_t phy_table_avail(void)
{
	if (phy_map == MAP_FAILED || phy_map_offset > phy_map_len)
		return 0;

	return phy_map_len - phy_map_offset;
}

os_access_obj_t *init_os_access_obj_mem() {
	os_access_obj_t *obj = malloc(sizeof(os_access_obj_t));

	if (obj == NULL)
		return NULL;

	memset(obj, 0, sizeof(os_access_obj_t));

	obj->access.mem.pci_acc = pci_alloc();
	if (!obj->access.mem.pci_acc) {
		fprintf(stderr, "pci_alloc failed\n");
		goto err_exit;
	}

	pci_init(obj->access.mem.pci_acc);

	obj->access.mem.pci_dev = pci_get_dev(obj->access.mem.pci_acc, 0, 0, 0, 0);
	if (!obj->access.mem.pci_dev) {
		fprintf(stderr, "Unable to get pci device\n");
		pci_cleanup(obj->access.mem.pci_acc);
		obj->access.mem.pci_acc = NULL;
		goto err_exit;
	}

	pci_fill_info(obj->access.mem.pci_dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);
	return obj;

err_exit:
	free(obj);
	return NULL;
}

/*
 * Map the physical PM table window.
 *
 * Fixes over the previous version:
 *  - `fd > 0` rejected a perfectly valid fd 0 (possible when stdin is closed).
 *  - the offset was cast to `long` and passed unaligned; mmap() requires a
 *    page-aligned offset, so any non-page-aligned table address failed with
 *    EINVAL instead of being handled.
 *  - the mapping was always exactly 0x1000 bytes even though the caller may
 *    copy more than that out of it (see copy_pm_table_mem).
 *  - O_CLOEXEC was missing.
 */
int init_mem_obj_mem(RA_UNUSED os_access_obj_t *os_access, const uintptr_t physAddr, const size_t size) {
	const size_t pgsz = page_size();
	const off_t aligned = (off_t)(physAddr & ~(uintptr_t)(pgsz - 1));
	const size_t offset = (size_t)(physAddr - (uintptr_t)aligned);
	const size_t want = (size && size <= RYZENADJ_MAX_TABLE_SIZE) ? size : RYZENADJ_MAX_TABLE_SIZE;
	const size_t len = ((offset + want + pgsz - 1) / pgsz) * pgsz;
	int dev_mem_fd;

	/* a fresh mapping replaces any previous one instead of leaking it */
	unmap_phy();

	// It is too complicated to check PAT, CONFIG_NONPROMISC_DEVMEM, CONFIG_STRICT_DEVMEM or other dependencies, just try to open /dev/mem
	dev_mem_fd = open("/dev/mem", O_RDONLY | O_CLOEXEC);
	if (dev_mem_fd < 0) {
		DBG("failed to open /dev/mem: %s\n", strerror(errno));
		return -1;
	}

	phy_map = mmap(NULL, len, PROT_READ, MAP_SHARED, dev_mem_fd, aligned);
	close(dev_mem_fd);

	if (phy_map == MAP_FAILED) {
		DBG("failed to mmap /dev/mem at 0x%llx: %s\n",
		    (unsigned long long)aligned, strerror(errno));
		return -1;
	}

	phy_map_len = len;
	phy_map_offset = offset;
	return 0;
}

void free_os_access_obj_mem(os_access_obj_t *obj) {
	unmap_phy();

	if (obj == NULL)
		return;

	if (obj->access.mem.pci_dev)
		pci_free_dev(obj->access.mem.pci_dev);

	if (obj->access.mem.pci_acc)
		pci_cleanup(obj->access.mem.pci_acc);

	free(obj);
}

uint32_t smn_reg_read_mem(const os_access_obj_t *obj, const uint32_t addr) {
	if (!pci_write_long(obj->access.mem.pci_dev, NB_PCI_REG_ADDR_ADDR, addr & ~(uint32_t)0x3))
		smn_io_set_error();
	return pci_read_long(obj->access.mem.pci_dev, NB_PCI_REG_DATA_ADDR);
}

void smn_reg_write_mem(const os_access_obj_t *obj, const uint32_t addr, const uint32_t data) {
	if (!pci_write_long(obj->access.mem.pci_dev, NB_PCI_REG_ADDR_ADDR, addr) ||
	    !pci_write_long(obj->access.mem.pci_dev, NB_PCI_REG_DATA_ADDR, data))
		smn_io_set_error();
}

int copy_pm_table_mem(RA_UNUSED const os_access_obj_t *obj, void *buffer, const size_t size) {
	const void *src = phy_table_ptr();

	if (src == NULL) {
		DBG("failed to get pm_table from /dev/mem\n");
		return -1;
	}

	/*
	 * Never read past the end of the mapping. Previously this copied `size`
	 * bytes unconditionally out of a fixed 0x1000 window, so any table larger
	 * than one page walked off the mapping and took the process down.
	 */
	if (size > phy_table_avail()) {
		DBG("pm_table read of %zu bytes exceeds mapped window of %zu bytes\n",
		    size, phy_table_avail());
		return -1;
	}

	memcpy(buffer, src, size);
	return 0;
}

int compare_pm_table_mem(const void *buffer, const size_t size) {
	const void *src = phy_table_ptr();

	/*
	 * The old version dereferenced phy_map unconditionally, so a compare
	 * before a successful mmap dereferenced MAP_FAILED ((void *)-1).
	 * Report "different" when we cannot read, so the caller re-transfers.
	 */
	if (src == NULL || size > phy_table_avail())
		return -1;

	return memcmp(buffer, src, size);
}
