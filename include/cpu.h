#ifndef CPU_H
#define CPU_H

static inline unsigned int cpu_get_id(void){
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned int)(mpidr & 0xFFUL);
}

static inline void cpu_enable_smp_coherency(void){
    // Cortex-A53 requires CPUECTLR_EL1.SMPEN=1 for hardware data-cache
    // coherency across cores. Some firmware enables this, but we set it
    // explicitly to avoid long-run SMP instability.
    unsigned long v;
    asm volatile("mrs %0, s3_1_c15_c2_1" : "=r"(v));
    v |= (1UL << 6); // SMPEN
    asm volatile("msr s3_1_c15_c2_1, %0" : : "r"(v));
    asm volatile("isb");
    asm volatile("dmb ish");
}

#endif
