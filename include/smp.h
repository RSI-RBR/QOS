#ifndef SMP_H
#define SMP_H

extern volatile unsigned int smp_boot_release_mask;

void smp_release_secondary_cores(void);

#endif
