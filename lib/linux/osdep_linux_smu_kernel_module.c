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
	obj->access.kmod.raw_cmd_fd = -1;

	/*
	 * Only the SMN/mailbox interface is mandatory: that is what every
	 * adjustment (--fast-limit, --stapm-limit, undervolting, ...) goes
	 * through.  The PM table is telemetry only (--info / --dump-table) and
	 * ryzen_smu does not create the pm_table* attributes at all when it
	 * cannot resolve the DRAM base address for the running CPU.  Treat a
	 * missing/unreadable PM table as "no telemetry", not as a fatal init
	 * error, otherwise RyzenAdj refuses to do the one thing it can still do.
	 */
	if (get_pm_table_size(&obj->access.kmod.pm_table_size) != 0) {
		DBG("continuing without PM table support (telemetry unavailable)\n");
		obj->access.kmod.pm_table_size = 0;
	}

	obj->access.kmod.smn_fd = open("/sys/kernel/ryzen_smu_drv/smn", O_RDWR | O_CLOEXEC);
	if (obj->access.kmod.smn_fd == -1) {
		DBG("failed to open smn fd: %s\n", strerror(errno));
		goto err_exit;
	}

	/* optional: atomic mailbox interface, serialised with the driver's own SMU use */
	obj->access.kmod.raw_cmd_fd = open("/sys/kernel/ryzen_smu_drv/smu_raw_cmd", O_RDWR | O_CLOEXEC);
	if (obj->access.kmod.raw_cmd_fd == -1)
		DBG("smu_raw_cmd not available (%s), using SMN register path\n", strerror(errno));

	if (obj->access.kmod.pm_table_size != 0) {
		obj->access.kmod.pm_table_fd = open("/sys/kernel/ryzen_smu_drv/pm_table",
						    O_RDONLY | O_CLOEXEC);
		if (obj->access.kmod.pm_table_fd == -1) {
			DBG("failed to open pm_table fd: %s\n", strerror(errno));
			obj->access.kmod.pm_table_size = 0;
		}
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

	if (obj->access.kmod.raw_cmd_fd >= 0)
		close(obj->access.kmod.raw_cmd_fd);

	free(obj);
}

uint32_t smn_reg_read_kmod(const os_access_obj_t *obj, const uint32_t addr) {
	uint32_t result = 0;

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
		return 0;
	}

	if (write_full(obj->access.kmod.smn_fd, &addr, sizeof(addr)) != 0) {
		DBG("%s: write error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
		return 0;
	}

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
		return 0;
	}

	if (read_full(obj->access.kmod.smn_fd, &result, sizeof(result)) != 0) {
		DBG("%s: read error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
		return 0;
	}

	return result;
}

void smn_reg_write_kmod(const os_access_obj_t *obj, const uint32_t addr, const uint32_t data) {
	const uint32_t write_buffer[2] = { addr, data };

	if (lseek(obj->access.kmod.smn_fd, 0, SEEK_SET) == (off_t)-1) {
		DBG("%s: lseek error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
		return;
	}

	if (write_full(obj->access.kmod.smn_fd, write_buffer, sizeof(write_buffer)) != 0) {
		DBG("%s: error: %s\n", __func__, strerror(errno));
		smn_io_set_error();
	}
}

int copy_pm_table_kmod(const os_access_obj_t *obj, void *buffer, const size_t size) {
	if (obj->access.kmod.pm_table_fd < 0 || obj->access.kmod.pm_table_size == 0) {
		DBG("PM table is not exported by ryzen_smu on this system\n");
		return -1;
	}

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

/* ryzen_smu SMU_Return_* values that don't exist in the SMU itself */
#define RSMU_RET_CMD_TIMEOUT 0xFB

int smu_raw_cmd_kmod(const os_access_obj_t *obj, const uint32_t msg, const uint32_t rep,
		     const uint32_t arg_base, const uint32_t id, smu_service_args_t *args,
		     uint32_t *response) {
	const int fd = obj->access.kmod.raw_cmd_fd;
	const uint32_t req[10] = { msg, rep, arg_base, id,
				   args->arg0, args->arg1, args->arg2,
				   args->arg3, args->arg4, args->arg5 };
	uint32_t res[7];

	if (fd < 0)
		return -1;

	if (lseek(fd, 0, SEEK_SET) == (off_t)-1 || write_full(fd, req, sizeof(req)) != 0) {
		if (errno == EINVAL) {
			/* mailbox outside the driver's allowed window: use SMN path */
			DBG("%s: driver rejected mailbox 0x%x, falling back\n", __func__, msg);
			return -1;
		}
		DBG("%s: write error: %s\n", __func__, strerror(errno));
		*response = REP_MSG_Failed;
		return 0; /* may or may not have executed: never retry blindly */
	}

	if (lseek(fd, 0, SEEK_SET) == (off_t)-1 || read_full(fd, res, sizeof(res)) != 0) {
		DBG("%s: read error: %s\n", __func__, strerror(errno));
		/* EAGAIN: another process overwrote the result slot */
		*response = errno == EAGAIN ? REP_MSG_CmdRejectedBusy : REP_MSG_Failed;
		return 0;
	}

	switch (res[0]) {
	case REP_MSG_OK:
		args->arg0 = res[1]; args->arg1 = res[2]; args->arg2 = res[3];
		args->arg3 = res[4]; args->arg4 = res[5]; args->arg5 = res[6];
		/* fall through */
	case REP_MSG_Failed:
	case REP_MSG_UnknownCmd:
	case REP_MSG_CmdRejectedPrereq:
	case REP_MSG_CmdRejectedBusy:
		*response = res[0];
		break;
	case RSMU_RET_CMD_TIMEOUT:
		*response = REP_MSG_Timeout;
		break;
	default: /* PCI failure / invalid argument inside the driver */
		*response = REP_MSG_Failed;
		break;
	}
	return 0;
}
