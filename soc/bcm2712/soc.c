#include "platform/soc.h"

const char* soc_name(void){
    return "bcm2712";
}

unsigned int soc_core_count(void){
    return 4u;
}
