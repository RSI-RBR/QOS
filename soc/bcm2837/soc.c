#include "platform/soc.h"

const char* soc_name(void){
    return "bcm2837";
}

unsigned int soc_core_count(void){
    return 4u;
}
