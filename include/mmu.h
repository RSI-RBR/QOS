#ifndef MMU_H
#define MMU_H

#define QOS_USER_GUARD_PAGE_BYTES 4096UL
#define QOS_USER_STACK_BYTES (128UL * 1024UL)

void mmu_init(void);
void mmu_enable_secondary(void);
void mmu_map_device_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_code_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_data_region(unsigned long pa_start, unsigned long size);
void mmu_map_kernel_private_region(unsigned long pa_start, unsigned long size);
void mmu_tlb_shootdown_all(void);
void mmu_handle_ipi(void);
void mmu_process_spaces_reset(void);
int mmu_process_space_create(int pid,
                             unsigned long user_pa_start,
                             unsigned long user_size,
                             unsigned long user_rw_offset,
                             unsigned long user_rw_size);
void mmu_process_space_destroy(int pid);
void mmu_switch_to_pid(int pid);

#endif
