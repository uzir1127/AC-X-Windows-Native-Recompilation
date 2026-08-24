#ifndef PSPRECO_THREADMAN_H
#define PSPRECO_THREADMAN_H

#include "pspreco.h"

void sysmem_set_partition_begin(u32 module_end);
u32  sysmem_alloc_high(const char *name, u32 size);
int  sysmem_free_at(u32 addr);

int  intrman_dispatch(CpuCtx *c, u8 *ram, u32 intr);
int  intrman_vblank_intr(void);

void display_vblank(CpuCtx *c, u8 *ram);
u32  display_frame_us(void);

int  ge_deliver_interrupts(CpuCtx *c, u8 *ram);
void ge_report_command_usage(void);

void threadman_wait_vblank(void);

void threadman_init(u8 *ram, const CpuCtx *boot);
void threadman_run(void);
void threadman_yield(void);
void threadman_block_us(u64 usec);
u64  threadman_now_us(void);
u64  threadman_vblank_count(void);
void threadman_advance_us(u64 usec);
void threadman_note_hle_call(void);
int  threadman_current_uid(void);
int  threadman_trace_context(void);
void threadman_suspend_intr(void);
void threadman_resume_intr(void);
void threadman_request_exit(void);

int  threadman_invoke_callback(CpuCtx *c, u8 *ram, int cbid, u32 a1, u32 a2);

void threadman_notify_callback(int cbid, u32 a1, u32 a2);
int  threadman_deliver_callbacks(CpuCtx *c, u8 *ram);

#endif
