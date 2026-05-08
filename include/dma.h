#ifndef DMA_H
#define DMA_H

void dma_init(void);
void dma_set_enabled(int enabled);
int dma_is_enabled(void);
unsigned int dma_failure_count(void);
int dma_memcpy_2d(void* dst,
                  unsigned int dst_stride,
                  const void* src,
                  unsigned int src_stride,
                  unsigned int row_bytes,
                  unsigned int rows);

#endif
