#ifndef MMU_H
#define MMU_H

void mmu_init(void);
void mmu_enable_secondary(void);
void mmu_map_device_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_code_region(unsigned long pa_start, unsigned long size);
void mmu_map_user_data_region(unsigned long pa_start, unsigned long size);
void mmu_map_kernel_private_region(unsigned long pa_start, unsigned long size);
void mmu_tlb_shootdown_all(void);
void mmu_handle_ipi(void);

#endif
