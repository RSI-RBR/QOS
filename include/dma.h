#ifndef DMA_H
#define DMA_H

void dma_init(void);
int dma_memcpy_2d(void* dst,
                  unsigned int dst_stride,
                  const void* src,
                  unsigned int src_stride,
                  unsigned int row_bytes,
                  unsigned int rows);

#endif
