#ifndef SMP_H
#define SMP_H

void smp_release_secondary_cores(void);
void smp_mark_core_online(unsigned int core_id);
unsigned int smp_online_mask(void);

#endif
