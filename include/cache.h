#ifndef CACHE_H
#define CACHE_H

void clean_data_cache(void);
void invalidate_instruction_cache(void);
void clean_invalidate_data_cache_range(unsigned long start, unsigned long size);

#endif
