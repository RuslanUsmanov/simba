/**
 * @file linux/gnu/thrd_port.i
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

#define THRD_MONITOR_STACK_MAX 256

#undef BIT

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern void thrd_port_main();

static struct thrd_t main_thrd __attribute__ ((section (".main_thrd")));

xSemaphoreHandle thrd_idle_sem;

static void thrd_port_init_main(struct thrd_port_t *port)
{
    vSemaphoreCreateBinary(thrd_idle_sem);
    xSemaphoreTake(thrd_idle_sem, portMAX_DELAY);
}

static int thrd_port_spawn(struct thrd_t *thrd_p,
                           void *(*main)(void *),
                           void *arg_p,
                           void *stack_p,
                           size_t stack_size)
{
    struct thrd_port_context_t *context_p;

    /* Put the context at the top of the stack. */
    context_p = (struct thrd_port_context_t *)
        ((((uintptr_t)stack_p) + stack_size - sizeof(*context_p) - 32) & 0xfffffff0);

    context_p->ps = (PS_WOE | PS_UM | PS_CALLINC(1) | PS_INTLEVEL(3));
    context_p->a0 = (uint32_t)thrd_port_main;
    /* The window underflow handler will load those values from the
       stack into a0-a3 and then call thrd_port_main(). */
    ((uint32_t *)context_p)[-1] = (uint32_t)main;      // a3
    ((uint32_t *)context_p)[-2] = (uint32_t)arg_p;     // a2
    ((uint32_t *)context_p)[-3] = (uint32_t)context_p; // a1
    ((uint32_t *)context_p)[-4] = 0; /* a0, thrd_port_main() will not
                                        return so the return address
                                        may have any value. */

    thrd_p->port.context_p = context_p;

    return (0);
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void thrd_port_idle_wait(struct thrd_t *thrd_p)
{
    /* Yield the Simba FreeRTOS thread and wait for an interrupt to
       occur. The interrupt handlers signals on this semaphore when a
       thread has been resumed and should be scheduled. */
    xSemaphoreTake(thrd_idle_sem, portMAX_DELAY);

    /* Add this thread to the ready list and reschedule. */
    sys_lock();
    thrd_p->state = THRD_STATE_READY;
    scheduler_ready_push(thrd_p);
    thrd_reschedule();
    sys_unlock();
}

static void thrd_port_suspend_timer_callback(void *arg_p)
{
    struct thrd_t *thrd_p = arg_p;

    /* Push thread on scheduler ready queue. */
    thrd_p->err = -ETIMEDOUT;
    thrd_p->state = THRD_STATE_READY;
    scheduler_ready_push(thrd_p);
}

static void thrd_port_tick(void)
{
    xSemaphoreGiveFromISR(thrd_idle_sem, NULL);
}

static void thrd_port_cpu_usage_start(struct thrd_t *thrd_p)
{
}

static void thrd_port_cpu_usage_stop(struct thrd_t *thrd_p)
{
}

#if CONFIG_MONITOR_THREAD == 1

static float thrd_port_cpu_usage_get(struct thrd_t *thrd_p)
{
    return (0.0);
}

static void thrd_port_cpu_usage_reset(struct thrd_t *thrd_p)
{
}

#endif