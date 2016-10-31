/**
 * @file linux/gnu/thrd_port.S
 *
 * @section License
 * Copyright (C) 2014-2016, Erik Moqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * This file is part of the Simba project.
 */

#define WOE_MASK                            0x00040000

.extern sys_unlock
.extern terminate

/**
 * Perform a thread context switch. Must be called by call4/8/12
 * instructions (so called windowed ABI).
 *
 * @param[in] a0 Return address.
 * @param[in] a1 Stack address.
 * @param[in] a2 Thread to swap in.
 * @param[in] a3 Thread to swap out.
 */
    .globl  thrd_port_swap
    .type   thrd_port_swap,@function
    .align  4
thrd_port_swap:
    entry   a1, 32

    /* Store registers on the stack. The windowed ABI stores most
       registers automatically. */
    rsr     a6, PS
    s32i    a0, a1, 0
    s32i    a6, a1, 4

    /* a2 and a3 are clobbered by xthal_window_spill_nw, so store the
       in and out thread pointers in a4 and a5. */
    mov     a4, a2
    mov     a5, a3

   /* The register window for the out thread must be stored in the
      save area on the stack for each frame. Underflow exceptions will
      restore the register window when registers are accessed. */
    movi    a7, ~WOE_MASK /* Disable register window overflow
                             detection. */
    and     a7, a6, a7
    wsr     a7, PS
    rsync
    call0   xthal_window_spill_nw

    /* Store swapped out-threads' stack address in the thread
       context. */
    s32i    a1, a5, 8

    /* Load swapped in-threads' stack address from the thread
       context. */
    l32i    a1, a4, 8

    /* Restore registers from the in-thread stack. */
    l32i    a6, a1, 4
    l32i    a0, a1, 0
    wsr     a6, PS

    retw.n

/**
 * Entry function of all threads except the main thread. Calls the
 * thread main function with argument.
 */
    .globl  thrd_port_main
    .type   thrd_port_main,@function
    .literal_position
    .align  4
thrd_port_main:
    //call4   sys_unlock

    /* Call the thread main function with argument. The main function
       and its argument is stored on the stack in the spawn
       function. */
    mov     a6, a2
    callx4   a3

    /* Thread termination. */
    call4   terminate