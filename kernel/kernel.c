#include "uart.h"
#include "shell.h"
#include "memory.h"
//#include "task.h"
#include "framebuffer.h"
#include "context.h"
#include "api.h"
#include "loader.h"
#include "process.h"
#include "fat32.h"
//#include "sd.h"
#include "gpio.h"
#include "blockdev.h"
//#include "clock.h"
#include "mailbox.h"
#include "debug.h"
#include "interrupt.h"
#include "mmu.h"
#include "net.h"
#include "usb_host.h"
#include "arp.h"


//extern kernel_api_t kapi;

//static unsigned char sector[512];

void memzero(unsigned long start, unsigned long size){
    for (unsigned long i = 0; i < size; i++)
        ((char*)start)[i] = 0;
}

void test_task(void *arg){
    char id = (char)(unsigned long)arg;

    uart_puts("Task ");
    uart_send(id);
    uart_puts(" running\n");
}

static void shell_process_entry(void){
    shell_init();
    shell_run();
}

static int create_boot_shell_process(void){
    // SHELL.BIN in FAT 8.3 format.
    loaded_program_t shell_prog = load_program_from_sd_named("SHELL   BIN");
    if (shell_prog.entry){
        int pid = process_create_loaded(shell_prog);
        if (pid >= 0){
            uart_puts("User shell started as PID ");
            uart_send('0' + pid);
            uart_puts("\n");
            return pid;
        }

        if (shell_prog.heap_allocated){
            kfree_secure(shell_prog.memory, shell_prog.size);
        } else{
            loader_free_program_memory(shell_prog.memory, shell_prog.size);
        }
        uart_puts("User shell create failed; falling back to kernel shell.\n");
    } else{
        uart_puts("SHELL.BIN not found; falling back to kernel shell.\n");
    }

    return process_create(shell_process_entry);
}

extern unsigned long stack_bottom;

void kernel_main(void){
    // Enable FP/ASIMD at EL1 to avoid EC=0x07 traps on generated code paths.
    asm volatile(
        "mrs x0, cpacr_el1\n"
        "orr x0, x0, #(3 << 20)\n"
        "msr cpacr_el1, x0\n"
        "isb\n"
        :
        :
        : "x0");

    uart_init();
    uart_puts("Uart initialized!\n");
    (void)mailbox_power_on_usb();

    mmu_init();
    uart_puts("MMU (phase 1 identity map) enabled.\n");
    loader_mmu_init_pool();

    uart_puts("STACK GUARD INIT = ");
    uart_puthex(*(unsigned long*)&stack_bottom);
    uart_puts("\n");
//    check_stack();

    memory_init();
    uart_puts("Memory initialized!\n");
    check_stack();

    process_init();
    interrupt_init();
    enable_interrupts();
    uart_puts("Interrupt system initialized!\n");

    if (usb_host_init() != 0){
        uart_puts("USB host init failed (network over onboard ETH unavailable).\n");
    } else{
        if (usb_host_enumerate_root_device() == 0){
            usb_root_device_info_t root_info;
            if (usb_host_get_root_device_info(&root_info) == 0){
                uart_puts("USB: enumerated root VID=");
                uart_puthex(root_info.vid);
                uart_puts(" PID=");
                uart_puthex(root_info.pid);
                uart_puts(" CLASS=");
                uart_puthex(root_info.dev_class);
                uart_puts(" CFG=");
                uart_puthex(root_info.config_value);
                uart_puts(root_info.configured ? " (set)\n" : " (not set)\n");

                if (root_info.vid == 0x0424 && root_info.pid == 0x9514){
                    uart_puts("USB: LAN9514 hub detected; next phase is hub downstream enumeration.\n");
                    if (root_info.child_present){
                        uart_puts("USB: LAN9514 child VID=");
                        uart_puthex(root_info.child_vid);
                        uart_puts(" PID=");
                        uart_puthex(root_info.child_pid);
                        uart_puts(" CLASS=");
                        uart_puthex(root_info.child_class);
                        uart_puts(root_info.child_configured ? " (configured)\n" : " (not configured)\n");
                    }
                }
            }
        } else{
            uart_puts("USB: root enumeration failed.\n");
            unsigned char dev_desc[18];
            if (usb_host_read_device_descriptor(dev_desc, sizeof(dev_desc)) == 0){
                uart_puts("USB: fallback desc VID=");
                uart_puthex((unsigned int)dev_desc[9] << 8 | dev_desc[8]);
                uart_puts(" PID=");
                uart_puthex((unsigned int)dev_desc[11] << 8 | dev_desc[10]);
                uart_puts("\n");
            }
        }
    }

    if (net_init() != 0){
        uart_puts("NET init failed (continuing without NIC).\n");
    } else{
        net_dump_stats();
    }

    fb_init();
    mmu_map_device_region(fb_get_base(), (unsigned long)fb_get_pitch() * (unsigned long)fb_get_height());
    fb_init_buffers();
    uart_puts("Frame buffer initialized!\n");

//    kapi.clear(0x00000000);
    fb_clear(0x00000000);
//    kapi.draw_rect(100, 100, 500, 300, 0x00FFFFFF);

    uart_puts("Kernel booted successfully!\n");

    // Kernel-level network identity.
    // Adjust local_ip to your subnet as needed.
    static const unsigned char local_mac[6] = {0x02, 0x51, 0x4F, 0x53, 0x00, 0x01};
    static const unsigned char local_ip[4] = {10, 0, 0, 88};
    static const unsigned char router_ip[4] = {10, 0, 0, 1};
    arp_set_local_interface(local_mac, local_ip);
    arp_set_periodic_target(router_ip, 2000); // 2s retry
    uart_puts("ARP interface configured (local 10.0.0.88)\n");

    // -----------------------------
    // OPTION 1: RUN SHELL (RECOMMENDED)
    // -----------------------------

//    program_entry_t prog;
//    prog = load_program_from_sd();

//    if (prog){
//        void *stack = alloc_stack();
//        if (!stack){
//            uart_puts("No stack available!\n");
//            return;
//        }
//        run_program(prog, stack, &kapi);
//        free_stack(stack);
//    }

//    uart_puts("\n--- START SD PIPELINE ---\n");
//    extern void sdhost_reset(void);
//    extern int sdhost_cmd(unsigned int cmd, unsigned int arg);
//    extern int sdhost_init_card(void);
//    extern int sdhost_read_block(unsigned int lba, unsigned char* buffer);
    if (blockdev_init() != 0){
        uart_puts("Blockdev init failed!\n");
        return;
    }
    uart_puts("Storage init OK...\n");
//    check_stack();
//    sdhost_read_block(0, sector);
//    uart_puts("First read OK\n");
//    sdhost_read_block(0, sector);
//    uart_puts("Second read OK\n");//    return;
//    if (sdhost_read_block(2048, sector) != 0){
//        uart_puts("READ FAILED!\n");
//        return;
//    }

//    uart_puts("Read success. First 16 bytes: \n");

//    for (int i = 0; i < 16; i++){
//        uart_puthex(sector[i]);
//    } uart_puts("\n");

//    static unsigned char prog[8192];
//
//    extern unsigned long bss_end;
//    extern unsigned long stack_top;
//    extern unsigned long stack_bottom;
//    uart_puts("bss_end = ");
//    uart_puthex((unsigned long)&bss_end);
//    uart_puts("\n");
//    uart_puts("stack_bottom = ");
//    uart_puthex((unsigned long)&stack_bottom);
//    uart_puts("\n");
//    uart_puts("stack_top = ");
//    uart_puthex((unsigned long)&stack_top);
//    uart_puts("\n");
//
//    check_stack();
//
//    if (fat32_init() != 0){
//        uart_puts("FAT init failed!\n");
//        return;
//    }
//    uart_puts("FAT INITIALIZED!\n");
//    int size = fat32_read_file("PROGRAM BIN", prog, sizeof(prog));
//
//    if (size < 0){
//        uart_puts("File read failed.\n");
//        return;
//    }
//
//    uart_puts("PROGRAM.BIN loaded, size = ");
//    uart_puthex(size);
//    uart_puts("\n");

//    uart_puts("--- END SD PIPELINE ---\n");

//    unsigned long entry = load_program_from_sd();
//    if (entry){
//        execute_program(entry);
//    } else{
//        uart_puts("No valid program loaded\n");
//        return;
//    }
//    
    
    int shell_pid = create_boot_shell_process();
    if (shell_pid < 0){
        uart_puts("Failed to create shell process\n");
        return;
    }

    // -----------------------------
    // OPTION 2: TASK DEMO (COMMENTED)
    // -----------------------------
    /*
    task_create(test_task, (void*)'A');
    task_create(test_task, (void*)'B');
    task_run_all();
    */

    // never reach here normally
    while (1){
        net_poll();
        if (scheduler_has_runnable()){
            scheduler_run_once();
        } else{
            asm volatile("wfi");
        }
    }
}
