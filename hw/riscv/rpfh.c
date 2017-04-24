/*
 * QEMU RISC-V Remote page fault handler emulation
 *
 * Author: Emmanuel Amaro amaro@berkeley.edu
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

#include "qemu/osdep.h"
#include "hw/riscv/rpfh.h"
#include "target-riscv/cpu.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include <inttypes.h>

#define PFA_INT_BASE            0x0
#define PFA_INT_FREEPAGE        (PFA_INT_BASE)
#define PFA_INT_EVICTPAGE       (PFA_INT_BASE + 8)

static RPFHState *rpfhstate;
static void *evicted_page;
static uint64_t evicted_pte;
static uintptr_t gpfreeframe; // points to guest physical free frame

/* fulfill the fetch page by copying the data in evicted_page to the
  gpfreeframe paddr */
void rpfh_fetch_page(CPURISCVState *env, target_ulong vaddr, hwaddr *paddr_res,
    target_ulong *pte)
{
    uint32_t asid = env->sptbr >> (TARGET_PHYS_ADDR_SPACE_BITS - PGSHIFT);
    printf("rpfh_fetch_page sptbr=%lx asid=%x vaddr=%lx\n", env->sptbr, asid, vaddr);

    // compute the host address for gpfreeframe
    uint64_t *frame_addr = (uint64_t *) gpaddr_to_hostaddr(gpfreeframe, rpfhstate);

    // copy from evicted_page to frame_addr
    memcpy(frame_addr, evicted_page, 4096);

    // update pte
    *paddr_res = gpfreeframe;
    target_ulong new_pte = 0;
    new_pte = (*paddr_res >> PGSHIFT) << PTE_PPN_SHIFT;
    new_pte = new_pte | (evicted_pte & 0xFF); // preserve the pte bits
    printf("new_pte=%lx\n", new_pte);
    *pte = new_pte;
}

/* guest physical address to host addr */
inline uintptr_t gpaddr_to_hostaddr(uintptr_t gpaddr, RPFHState *r) {
    return (uintptr_t) r->hostptr_guest_dram + (gpaddr & 0x7FFFFFFF);
}

/* evict the page, for now, store it in memory */
static void rpfh_evict_page(uint64_t pte_gpaddr, RPFHState *r) {
    printf("qemu rpfh evict page\n");
    // read pte
    uint64_t *pte = (uint64_t *) gpaddr_to_hostaddr(pte_gpaddr, r);
    uint64_t frame_gpaddr = (*pte >> 10) << 12; // the pte's physical address

    // set pte as remote
    *pte = *pte | PTE_REMOTE;

    // simulate remote memory, save page locally
    evicted_page = malloc(4096);
    uint64_t *frame_addr = (uint64_t *) gpaddr_to_hostaddr(frame_gpaddr, r);
    memcpy(evicted_page, frame_addr, 4096);
    evicted_pte = *pte;
}

/* process a new page published to be used by rpfh */
static void rpfh_freepage(uint64_t pte_gpaddr, RPFHState *r) {
    printf("rpfh_freepage, pte_gpaddr=%lx\n", pte_gpaddr);
    // the objective is to get the paddr from the pte, and store it

    uint64_t *pte = (uint64_t *) gpaddr_to_hostaddr(pte_gpaddr, r);
    uint64_t frame_gpaddr = (*pte >> 10) << 12;
    gpfreeframe = frame_gpaddr;
}

static void rpfh_queues_write(void *opaque, hwaddr mmioaddr,
                              uint64_t value, unsigned size)
{
    RPFHState *r = opaque;
    (void) r;

    if (mmioaddr == PFA_INT_FREEPAGE && value != 0) {
        rpfh_freepage(value, r);
    } else if (mmioaddr == PFA_INT_EVICTPAGE && value != 0) {
        rpfh_evict_page(value, r);
    } else if (value == 0) {
        printf("value = 0\n");
    } else {
        printf("not implemented\n");
        exit(1);
    }
}

static uint64_t rpfh_queues_read(void *opaque, hwaddr addr, unsigned size)
{
    printf("read from queue mmio\n");
    return 0;
}

// OS writes free pages rpfh can use,
// OS reads new pages brought in by rpfh
static const MemoryRegionOps rpfh_queue_ops[3] = {
    [DEVICE_LITTLE_ENDIAN] = {
        .read = rpfh_queues_read,
        .write = rpfh_queues_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
    },
};

void rpfh_init_mmio(MemoryRegion *guest_as, MemoryRegion *guest_dram)
{
    RPFHState *r;
    r = g_malloc0(sizeof(RPFHState));

    rpfhstate = r;
    r->guest_dram = guest_dram;
    r->hostptr_guest_dram = memory_region_get_ram_ptr(guest_dram);
    memory_region_init_io(&r->io, NULL,
                          &rpfh_queue_ops[DEVICE_LITTLE_ENDIAN],
                          r, "rpfh queues", RPFH_IO_SIZE);
    memory_region_add_subregion(guest_as, RPFH_IO_ADDR, &r->io);
}
