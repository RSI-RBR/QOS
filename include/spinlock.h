#ifndef SPINLOCK_H
#define SPINLOCK_H

typedef struct {
    volatile unsigned int v;
} spinlock_t;

void spinlock_init(spinlock_t* lock);
void spin_lock(spinlock_t* lock);
int spin_trylock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

unsigned long spin_lock_irqsave(spinlock_t* lock);
void spin_unlock_irqrestore(spinlock_t* lock, unsigned long daif);

#endif
