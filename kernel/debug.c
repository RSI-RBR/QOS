#include "debug.h"
#include "panic.h"

extern unsigned long stack_bottom;

void check_stack(void){
    unsigned long *guard = (unsigned long *)&stack_bottom;

    if (*guard != 0xAAAAAAAA){
        qos_panic("boot stack guard corrupted", __FILE__, __LINE__);
    }
}
