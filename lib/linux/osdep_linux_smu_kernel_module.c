// SPDX-License-Identifier: LGPL
/* Backend which uses the sysfs interface provided by the ryzen_smu kernel module. */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "osdep_linux_smu_kernel_module.h"

/*
 * Read exactly `len` bytes, restarting on EINTR and on short reads.
 * The sysfs binary attributes exposed by ryzen_smu are not guaranteed to be
 * served in a single read(); the old code checked only for -1, so a short read
 * left the tail of the PM table filled with stale/zero data and RyzenAdj
 * happily reported it as telemetry.
 */
static int read_full(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t done = 0;

	while (done < len) {
		const ssize_t n = read(fd, p + done, len - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0) {
			errno = EIO; /* short file */
			return -1;
		}

		done += (size_t)n;
	}

	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t done = 0;

	while (done < len) {
		const ssize_t n = write(fd, p + done, len - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		done += (size_t)n;
	}

	return 0;
}

/*
 * Returns 0 and stores the size on success, -1 on failure.
 *
 * The old signature returned uint32_t and used -1 as the error value, but the
 * caller compared the result against -1 *after* storing it in a size_t field:
 * (size_t)0xFFFFFFFF != (size_t)-1 on LP64, so the error branch was dead and a
 * failed read produced a 4 GiB "table size".
 */
static int get_pm_table_size(size_t *out)
{
	const int fd = open("/sys/kernel/ryzen_smu_drv/pm_table_size", O_RDONLY | O_CLOEXEC);
	uint32_t table_sz = 0;

	if (fd == -1) {
		DBG("failed to open pm_table_size: %s\n", strerror(errno));
		return -1;
	}

	if (read_full(fd, &table_sz, sizeof(table_sz)) != 0) {
		DBG("failed to retrieve PM table size: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);

	if (table_sz == 0 || table_sz > RYZENADJ_MAX_TABLE_SIZE) {
		DBG("ryzen_smu reported implausible PM table size: %u\n", table_sz);
		return -1;
	}

	*out = table_sz;
	return 0;
}

os_access_obj_t *init_os_access_obj_kmod() {
	os_access_obj_t *obj = malloc(sizeof(os_access_obj_t));

	if (obj == NULL)
		return NULL;

	memset(obj, 0, sizeof(os_access_obj_t));
	obj->access.kmod.smn_fd = -1;
	obj->access.kmod.pm_table_fd = -1;

	if (get_pm_table_size(&obj->access.kmod.pm_table_size) != 0)
		goto err_exit;

	obj->access.kmod.smn_fd = open("/sys/kernel/ryzen_smu_drv/smn", O_RDWR | O_CLOEXEC);
	if (obj->access.kmod.smn_fd == -1) {
		DBG("failed to open smn fd: %s\n", strerror(errno));
		goto err_exit;
	}

	obj->access.kmod.pm_table_fd = open("/sys/kernel/ryzen_smu_drv/pm_table", O_RDONLY | O_CLOEXEC);
	if (obj->access.kmod.pm_table_fd == -1) {
		DBG("failed to open pm_table fd: %s\n", strerror(errno));
		close(obj->access.kmod.smn_fd);
		goto err_exit;
	}

	return obj;

err_exit:
	free(obj);
	return NULL;
}

int init_mem_obj_kmod(RA_UNUSED os_access_obj_t *os_access, RA_UNUSED const uintptr_t physAddr,
		      RA_UNUSED const size_t size) {
	return 0;
}

void free_os_access_obj_kmod(os_access_obj_t *obj) {
	if (obj == NULL)
		return;

	if (obj->access.kmod.smn_fd >= 0)
		close(obj->access.kmod.smn_fd);

	if (obj->access.kmod.pm_table_fd >= 0)
		close(obj->access.kmod.pm_table_fd);

	free(obj);
}

uint32_t smn_reg_read_kmod(const os_access_obj_t *obj, const uint32_t addr) {
	uint32_t result = 0;

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		return 0;
	}

	if (write_full(obj->access.kmod.smn_fd, &addr, sizeof(addr)) != 0) {
		DBG("%s: write error: %s\n", __func__, strerror(errno));
		return 0;
	}

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		return 0;
	}

	if (read_full(obj->access.kmod.smn_fd, &result, sizeof(result)) != 0) {
		DBG("%s: read error: %s\n", __func__, strerror(errno));
		return 0;
	}

	return result;
}

void smn_reg_write_kmod(const os_access_obj_t *obj, const uint32_t addr, const uint32_t data) {
	const uint32_t write_buffer[2] = { addr, data };

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		return;
	}

	if (write_full(obj->access.kmod.smn_fd, write_buffer, sizeof(write_buffer)) != 0)
		DBG("%s: error: %s\n", __func__, strerror(errno));
}

int copy_pm_table_kmod(const os_access_obj_t *obj, void *buffer, const size_t size) {
	if (obj->access.kmod.pm_table_size < size) {
		DBG("PM table size too small: ryzenadj (%zu) | ryzen_smu (%zu)\n", size, obj->access.kmod.pm_table_size);
		return -1;
	}

	if (obj->access.kmod.pm_table_size != size) {
		DBG("PM table size mismatch (reading prefix): ryzenadj (%zu) | ryzen_smu (%zu)\n", size, obj->access.kmod.pm_table_size);
	}

	if (lseek(obj->access.kmod.pm_table_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		return -1;
	}

	if (read_full(obj->access.kmod.pm_table_fd, buffer, size) != 0) {
		DBG("%s: error: %s\n", __func__, strerror(errno));
		return -1;
	}

	return 0;
}

int compare_pm_table_kmod(RA_UNUSED const void *buffer, RA_UNUSED size_t size) {
	DBG("internal error: compare_pm_table() should never be called if ryzen_smu is loaded\n");
	return -1;
}
