#ifndef QOS_PLATFORM_MMIO_H
#define QOS_PLATFORM_MMIO_H

#include "platform/board_config.h"

#if defined(QOS_SOC_BCM2837)

#define QOS_MMIO_BASE              0x3F000000UL
#define QOS_MMIO_END               0x40200000UL
#define QOS_ARM_LOCAL_BASE         0x40000000UL
#define QOS_VC_BUS_UNCACHED_BASE   0xC0000000UL
#define QOS_VC_BUS_ARM_MASK        0x3FFFFFFFUL
#define QOS_LOW_PERIPHERAL_LIMIT   QOS_MMIO_BASE

#define QOS_DMA_BASE               (QOS_MMIO_BASE + 0x00007000UL)
#define QOS_CLOCK_BASE             (QOS_MMIO_BASE + 0x00101000UL)
#define QOS_IRQ_BASE               (QOS_MMIO_BASE + 0x0000B000UL)
#define QOS_MAILBOX_BASE           (QOS_MMIO_BASE + 0x0000B880UL)
#define QOS_GPIO_BASE              (QOS_MMIO_BASE + 0x00200000UL)
#define QOS_UART0_BASE             (QOS_MMIO_BASE + 0x00201000UL)
#define QOS_SDHOST_BASE            (QOS_MMIO_BASE + 0x00202000UL)
#define QOS_EMMC_BASE              (QOS_MMIO_BASE + 0x00300000UL)
#define QOS_USB_DWC2_BASE          (QOS_MMIO_BASE + 0x00980000UL)
#define QOS_V3D_MMIO_BASE          (QOS_MMIO_BASE + 0x00C00000UL)

#elif defined(QOS_SOC_BCM2712)

/*
 * Pi 5 is intentionally scaffolded but not enabled yet. Its interrupt,
 * PCIe/RP1, USB, and display paths differ enough from BCM2837 that guessing
 * addresses here would be worse than failing loudly.
 */
#error "BCM2712/Pi 5 platform MMIO map is not implemented yet."

#else
#error "Unknown QOS SoC. Select a supported board config."
#endif

#endif
