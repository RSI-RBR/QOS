#include "syscall.h"

/*
 * QOS starts user programs at program_main(void).
 * The private game can keep its desktop-style main(int argc, char** argv);
 * the game Makefile compiles that symbol as qf2d_main.
 */
extern int qf2d_main(int argc, char** argv);

void program_main(void){
    char arg0[] = "QF2D";
    char* argv[] = { arg0, 0 };
    int rc = qf2d_main(1, argv);

    qos_exit(rc);
}
