#include "syscall.h"

/*
 * QOS starts user programs at program_main(void).
 * The private game can keep its desktop-style main(int argc, char** argv);
 * the game Makefile compiles that symbol as qf2d_main.
 */
extern __attribute__((visibility("hidden"))) int qf2d_main(int argc, char** argv);

void program_main(void){
    int rc = qf2d_main(0, 0);

    qos_exit(rc);
}
