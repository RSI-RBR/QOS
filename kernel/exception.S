.global vectors

.align 11

vectors:
    b sync_el1t
    b .
    b .
    b .

    b sync_el1h
    b .
    b .
    b .

    b .
    b .
    b .
    b .

    b .
    b .
    b .
    b .

sync_el1t:
    b .

sync_el1h:
    // SVC handler
    bl handle_sync
    eret
