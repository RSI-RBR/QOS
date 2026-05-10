#ifndef MMU_H
#define MMU_H

#include "program.h"

void mmu_init(void);
void mmu_enable_secondary(void);
void mmu_map_device_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_code_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_data_region(unsigned long pa_start, unsigned long size);
void mmu_map_kernel_private_region(unsigned long pa_start, unsigned long size);
void mmu_tlb_shootdown_all(void);
void mmu_sync_local_tlb(void);
void mmu_handle_ipi(void);
void mmu_process_spaces_reset(void);
int mmu_process_space_create(int pid,
                             unsigned long user_pa_start,
                             unsigned long user_size,
                             unsigned long user_rw_offset,
                             unsigned long user_rw_size);
void mmu_process_space_destroy(int pid);
int mmu_process_map_framebuffer(int pid, unsigned long pa_start, unsigned long size);
void mmu_switch_to_pid(int pid);
void mmu_prepare_return_to_pid(int pid);
void mmu_debug_dump_current(unsigned long va);

#endif
