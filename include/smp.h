#ifndef SMP_H
#define SMP_H

void smp_release_secondary_cores(void);
void smp_mark_core_online(unsigned int core_id);
void smp_mark_core_offline(unsigned int core_id);
unsigned int smp_online_mask(void);
void smp_init_ipi_for_core(unsigned int core_id);
void smp_send_ipi(unsigned int target_core);
void smp_clear_ipi_for_core(unsigned int core_id);

#endif
