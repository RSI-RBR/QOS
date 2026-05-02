#ifndef CPU_H
#define CPU_H

static inline unsigned int cpu_get_id(void){
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned int)(mpidr & 0xFFUL);
}

#endif
