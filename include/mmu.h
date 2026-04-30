#ifndef MMU_H
#define MMU_H

void mmu_init(void);
void mmu_map_device_region(unsigned long pa_start, unsigned long size);

#endif
