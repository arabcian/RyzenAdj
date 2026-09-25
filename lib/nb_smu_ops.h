/* SPDX-License-Identifier: LGPL */
/* Copyright (C) 2018-2019 Jiaxun Yang <jiaxun.yang@flygoat.com> */
/* Ryzen NB SMU Service Request Operations */

#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Portable "this parameter is intentionally unused" marker.
 * The C23 [[maybe_unused]] spelling that used to be sprinkled over the OS
 * backends is a GCC/Clang extension in every pre-C23 mode and a hard error on
 * older compilers and MSVC.
 */
#if defined(__GNUC__) || defined(__clang__)
#define RA_UNUSED __attribute__((unused))
#else
#define RA_UNUSED
#endif

#ifdef NDEBUG
#define DBG(...)
#else
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#endif

enum SMU_TYPE{
	TYPE_MP1,
	TYPE_PSMU,
	TYPE_COUNT,
};

#define NB_PCI_REG_ADDR_ADDR			0xB8
#define NB_PCI_REG_DATA_ADDR			0xBC

/*
 * 0x0 is the "response register not written yet" state. We also use it as the
 * timeout marker: smu_service_req() only ever returns 0x0 when the SMU failed
 * to answer within SMU_RESP_TIMEOUT_MS.
 */
#define REP_MSG_Timeout               0x0
#define REP_MSG_OK                    0x1
#define REP_MSG_Failed                0xFF
#define REP_MSG_UnknownCmd            0xFE
#define REP_MSG_CmdRejectedPrereq     0xFD
#define REP_MSG_CmdRejectedBusy       0xFC

typedef struct _smu_service_args_t {
		uint32_t arg0;
		uint32_t arg1;
		uint32_t arg2;
		uint32_t arg3;
		uint32_t arg4;
		uint32_t arg5;
} smu_service_args_t;

typedef struct {
#ifdef _WIN32
	uint32_t pci_address;
	HINSTANCE inpoutDll;
#else
	union {
		struct {
			struct pci_access *pci_acc;
			struct pci_dev *pci_dev;
		} mem;
		struct {
			int smn_fd;
			int pm_table_fd;
			int raw_cmd_fd; /* ryzen_smu >= 0.1.9 smu_raw_cmd, -1 if absent */
			size_t pm_table_size;
		} kmod;
	} access;
#endif
} os_access_obj_t;

typedef struct _smu_t {
	os_access_obj_t *os_access;
	uint32_t msg;
	uint32_t rep;
	uint32_t arg_base;
} *smu_t;

os_access_obj_t *init_os_access_obj();
int init_mem_obj(os_access_obj_t *os_access, uintptr_t physAddr, size_t size);
int copy_pm_table(const os_access_obj_t *obj, void *buffer, size_t size);
int compare_pm_table(const void *buffer, size_t size);
void free_os_access_obj(os_access_obj_t *obj);

/* Upper bound for how long we busy-wait on the SMU response register. */
#define SMU_RESP_TIMEOUT_MS 1000

/*
 * Hard sanity bound on the PM table size. The largest table we know about is
 * 0xE50 bytes and unknown table versions fall back to a 0x1000 probe, so 8 KiB
 * leaves plenty of headroom while stopping a bogus SMU/sysfs value from
 * driving a huge allocation or an out-of-range mapping.
 */
#define RYZENADJ_MAX_TABLE_SIZE 0x2000u

uint32_t smn_reg_read(const os_access_obj_t *obj, uint32_t addr);
void smn_reg_write(const os_access_obj_t *obj, uint32_t addr, uint32_t data);
/*
 * Returns non-zero if any smn_reg_read/smn_reg_write failed since the last
 * call, and clears the flag. smn_reg_write() is void for ABI reasons, so this
 * is how a failed argument write is detected before the message id is sent.
 */
int smn_io_take_error(void);
/*
 * Run a whole mailbox transaction in the backend if it can do so atomically
 * (ryzen_smu smu_raw_cmd). Returns 0 and fills *response/args when it ran,
 * -1 when the backend has no such interface and the caller must fall back to
 * driving the registers itself.
 */
int smu_raw_cmd(const os_access_obj_t *obj, uint32_t msg, uint32_t rep, uint32_t arg_base,
		uint32_t id, smu_service_args_t *args, uint32_t *response);
bool is_using_smu_driver();

smu_t get_smu(os_access_obj_t *obj, int smu_type);
uint32_t smu_service_req(smu_t smu, uint32_t id, smu_service_args_t *args);
