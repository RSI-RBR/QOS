#ifndef DMA_H
#define DMA_H

void dma_init(void);
void dma_set_enabled(int enabled);
int dma_is_enabled(void);
unsigned int dma_failure_count(void);
unsigned int dma_last_cs(void);
unsigned int dma_last_debug(void);
unsigned int dma_transfer_count(void);
unsigned int dma_last_bytes(void);
unsigned int dma_last_clean_us(void);
unsigned int dma_last_wait_us(void);
unsigned int dma_last_total_us(void);
int dma_memcpy_to_bus(unsigned int dst_bus, const void* src, unsigned int bytes);
int dma_memcpy_2d_to_bus(unsigned int dst_bus,
                         unsigned int dst_stride,
                         const void* src,
                         unsigned int src_stride,
                         unsigned int row_bytes,
                         unsigned int rows);
int dma_memcpy_2d(void* dst,
                  unsigned int dst_stride,
                  const void* src,
                  unsigned int src_stride,
                  unsigned int row_bytes,
                  unsigned int rows);

#endif
