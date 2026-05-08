#include "uart.h"
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
#include "socket.h"
#include "console.h"
#include "cpu.h"
#include "smp.h"
#include "kernel_verify.h"
#include "crypto.h"
#include "aes_gcm.h"
#include "tls_record.h"
#include "x25519.h"
#include "pq_kem.h"
#include "tls_key_schedule.h"
#include "tls_handshake.h"
#include "tls_session.h"
#include "pq_sig.h"
#include "auth.h"
#include "remote_login.h"
#include "panic.h"
#include "terminal.h"


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
        uart_puts("User shell create failed; boot shell disabled by policy.\n");
    } else{
        uart_puts("SHELL.BIN not found; boot shell disabled by policy.\n");
    }

    return -1;
}

extern unsigned long stack_bottom;

void kernel_secondary_main(void){
    // Per-core EL1 init path for cores 1..3.
    asm volatile(
        "mrs x0, cpacr_el1\n"
        "orr x0, x0, #(3 << 20)\n"
        "msr cpacr_el1, x0\n"
        "isb\n"
        :
        :
        : "x0");

    mmu_enable_secondary();
    interrupt_init();
    smp_mark_core_online(cpu_get_id());
    enable_interrupts();

    while (1){
        asm volatile("wfi");
    }
}

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
    qos_stack_canary_init();
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
    crypto_init();
    uart_puts("Crypto initialized!\n");
    if (aes_gcm_self_test() == 0){
        uart_puts("AES-GCM self-test OK\n");
    } else{
        uart_puts("AES-GCM self-test FAILED\n");
    }
    if (tls13_record_self_test() == 0){
        uart_puts("TLS record self-test OK\n");
    } else{
        uart_puts("TLS record self-test FAILED\n");
    }
    int x25519_rc = x25519_self_test();
    if (x25519_rc == 0){
        uart_puts("X25519 self-test OK\n");
    } else{
        unsigned long x25519_abs = (x25519_rc < 0) ? (unsigned long)(-x25519_rc) : (unsigned long)x25519_rc;
        uart_puts("X25519 self-test FAILED rc=");
        uart_putdec(x25519_abs);
        uart_puts("\n");
    }
    if (pq_kem_mlkem768_available()){
        if (pq_kem_mlkem768_self_test() == 0){
            uart_puts("ML-KEM-768 self-test OK\n");
        } else{
            uart_puts("ML-KEM-768 self-test FAILED\n");
        }
    } else{
        uart_puts("ML-KEM-768 backend unavailable (X25519 fallback)\n");
    }
    if (pq_sig_self_test() == 0){
        uart_puts("PQ signature self-test OK\n");
    } else{
        uart_puts("PQ signature self-test FAILED\n");
    }
    if (tls13_key_schedule_self_test() == 0){
        uart_puts("TLS key schedule self-test OK\n");
    } else{
        uart_puts("TLS key schedule self-test FAILED\n");
    }
    if (tls13_handshake_self_test() == 0){
        uart_puts("TLS handshake scaffold self-test OK\n");
    } else{
        uart_puts("TLS handshake scaffold self-test FAILED\n");
    }

    process_init();
    interrupt_init();
    enable_interrupts();
    uart_puts("Interrupt system initialized!\n");

    if (usb_host_init() != 0){
        uart_puts("USB host init failed (network over onboard ETH unavailable).\n");
    } else{
        if (usb_host_enumerate_root_device() != 0){
            uart_puts("USB: root enumeration failed.\n");
        }
    }

    if (net_init() != 0){
        uart_puts("NET init failed (continuing without NIC).\n");
    }
    socket_layer_init();
    tls_session_layer_init();
    {
        int tsrc = tls_session_self_test();
        if (tsrc == 0){
            uart_puts("TLS session self-test OK\n");
        } else{
            unsigned long abs = (tsrc < 0) ? (unsigned long)(-tsrc) : (unsigned long)tsrc;
            uart_puts("TLS session self-test FAILED rc=");
            uart_putdec(abs);
            uart_puts("\n");
        }
    }
    console_init();

    fb_init();
    mmu_map_device_region(fb_get_base(), (unsigned long)fb_get_pitch() * (unsigned long)fb_get_height());
    fb_init_buffers();
    uart_puts("Frame buffer initialized!\n");

//    kapi.clear(0x00000000);
    fb_clear(0x00000000);
    terminal_init();
//    kapi.draw_rect(100, 100, 500, 300, 0x00FFFFFF);

    uart_puts("Kernel booted successfully!\n");

    smp_mark_core_online(cpu_get_id());
    smp_release_secondary_cores();
    uart_puts("SMP: released cores 1-3\n");
    for (unsigned int i = 0; i < 2000000u; i++){
        if (smp_online_mask() == 0x0Fu){
            break;
        }
        asm volatile("nop");
    }
    uart_puts("SMP: online mask=");
    uart_puthex(smp_online_mask());
    uart_puts("\n");

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

    int kv = kernel_verify_self();
    if (kv < 0){
        uart_puts("Kernel verify failed.\n");
        if (kernel_verify_enforce()){
            uart_puts("Kernel verify enforced: HALTING.\n");
            while (1){ asm volatile("wfi"); }
        }
        uart_puts("Kernel verify warn-only mode: continuing boot.\n");
    } else if (kv > 0){
        uart_puts("Kernel verify not provisioned yet.\n");
    }

    int kv_file = kernel_verify_storage_image();
    if (kv_file < 0){
        uart_puts("Kernel file verify failed.\n");
        if (kernel_verify_enforce()){
            uart_puts("Kernel verify enforced: HALTING.\n");
            while (1){ asm volatile("wfi"); }
        }
        uart_puts("Kernel file verify warn-only mode: continuing.\n");
    } else if (kv_file > 0){
        uart_puts("Kernel file verify not provisioned yet.\n");
    }

    {
        int kernel_trust_ok = (kv == 0 && kv_file == 0);
        if (!kernel_trust_ok){
            uart_puts("Boot security policy: kernel trust not established.\n");
            uart_puts("Boot security policy: local shell + remote login disabled.\n");
            while (1){
                asm volatile("wfi");
            }
        }
    }

    if (auth_init() != 0){
        uart_puts("AUTH init failed; remote login disabled.\n");
    } else{
        (void)remote_login_init();
    }
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
        uart_puts("No boot shell process available; halting in idle loop.\n");
        while (1){
            asm volatile("wfi");
        }
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
        if (scheduler_has_runnable()){
            scheduler_run_once();
        } else{
            asm volatile("wfi");
        }
    }
}
