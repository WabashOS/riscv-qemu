/*
 * QEMU RISCV Remote page fault handler
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef HW_RISCV_RPFH_H
#define HW_RISCV_RPFH_H 1

#include "hw/hw.h"
#include "sysemu/sysemu.h"
#include "exec/memory.h"
#include "target-riscv/cpu.h"

#define RPFH_QUEUES_ADDR      0x2000000
#define RPFH_QUEUES_SIZE      0x1000

typedef struct RPFHState {
    MemoryRegion io;
    MemoryRegion *guest_dram;
    MemoryRegion *hostptr_guest_dram;
} RPFHState;

typedef enum { evict, freepage } rpfh_op;

typedef struct rpfh_request {
    uint64_t pte_paddr;
    uint64_t vaddr;
    uint64_t paddr;
    uint32_t pid;
    rpfh_op op;
} rpfh_request;

void rpfh_fetch_page(CPURISCVState *env, target_ulong vaddr, hwaddr *paddr_res,
    target_ulong *pte);
void rpfh_init_mmio(MemoryRegion *guest_as, MemoryRegion *guest_dram);
uintptr_t gpaddr_to_hostaddr(uintptr_t gpaddr, RPFHState *r);

#endif
