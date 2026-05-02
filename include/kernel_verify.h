#ifndef KERNEL_VERIFY_H
#define KERNEL_VERIFY_H

// Returns:
//  0  verified OK
//  1  verification skipped/unprovisioned
// -1  verification failed
int kernel_verify_self(void);

// Non-zero means kernel should halt on verification failure.
int kernel_verify_enforce(void);

#endif
