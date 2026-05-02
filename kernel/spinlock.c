#include "spinlock.h"

void spinlock_init(spinlock_t* lock){
    if (!lock){
        return;
    }
    lock->v = 0;
    asm volatile("dmb ish" : : : "memory");
}

void spin_lock(spinlock_t* lock){
    unsigned int ldv;
    unsigned int st;
    const unsigned int one = 1u;
    if (!lock){
        return;
    }

    while (1){
        asm volatile("ldaxr %w0, [%1]" : "=&r"(ldv) : "r"(&lock->v) : "memory");
        if (ldv != 0u){
            asm volatile("clrex" : : : "memory");
            continue;
        }
        asm volatile("stxr %w0, %w1, [%2]"
                     : "=&r"(st)
                     : "r"(one), "r"(&lock->v)
                     : "memory");
        if (st == 0u){
            break;
        }
    }
    asm volatile("dmb ish" : : : "memory");
}

int spin_trylock(spinlock_t* lock){
    unsigned int ldv;
    unsigned int st = 1;
    const unsigned int one = 1u;
    if (!lock){
        return 0;
    }

    asm volatile("ldaxr %w0, [%1]" : "=&r"(ldv) : "r"(&lock->v) : "memory");
    if (ldv != 0u){
        asm volatile("clrex" : : : "memory");
        return 0;
    }
    asm volatile("stxr %w0, %w1, [%2]"
                 : "=&r"(st)
                 : "r"(one), "r"(&lock->v)
                 : "memory");

    if (st == 0u){
        asm volatile("dmb ish" : : : "memory");
        return 1;
    }
    return 0;
}

void spin_unlock(spinlock_t* lock){
    if (!lock){
        return;
    }
    asm volatile("dmb ish" : : : "memory");
    asm volatile("stlr wzr, [%0]" : : "r"(&lock->v) : "memory");
    asm volatile("sev" : : : "memory");
}

unsigned long spin_lock_irqsave(spinlock_t* lock){
    unsigned long daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" : : : "memory");
    spin_lock(lock);
    return daif;
}

void spin_unlock_irqrestore(spinlock_t* lock, unsigned long daif){
    spin_unlock(lock);
    asm volatile("msr daif, %0" : : "r"(daif) : "memory");
}
