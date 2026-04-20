#include "string.h"

int kstrlen(const char *str){
    int len = 0;
    while (str[len]) len ++;
    return len;
}

int kstrcmp(const char *a, const char *b){
    while(*a && (*a == *b)){
        a++;
        b++;
    }
    return *(unsigned char*)a - *(unsigned char*)b;
}

void kstrcpy(char *dest, const char *src){
    while(*src){
        *dest++ = *src++;
    }
    *dest = 0;
}
