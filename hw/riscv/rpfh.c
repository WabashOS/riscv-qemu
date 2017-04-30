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
#include "qemu/queue.h"
#include <inttypes.h>

#define PFA_INT_BASE            0x0
#define PFA_INT_FREEPAGE        (PFA_INT_BASE)
#define PFA_INT_EVICTPAGE       (PFA_INT_BASE + 8)
#define PFA_INT_NEWFRAME        (PFA_INT_BASE + 16)

static RPFHState *rpfhstate;

struct freeframe {
    uintptr_t gptr;
    QTAILQ_ENTRY(freeframe) link;
};

struct evictedframe {
    void *data;
    uint64_t pte;
    QTAILQ_ENTRY(evictedframe) link;
};

struct newframe {
    uint64_t pte;
    QTAILQ_ENTRY(newframe) link;
};

QTAILQ_HEAD(freeframe_head, freeframe) headff;
QTAILQ_HEAD(evictedframe_head, evictedframe) headef;
QTAILQ_HEAD(newframe_head, newframe) headnf;

/* Get the physical address of the page frame pointed to by pte */
static inline uintptr_t pte_frame(uint64_t *pte) {
  return (((*pte & ~PTE_REMOTE) >> PTE_PPN_SHIFT) << 12);
}

/* guest physical address to host addr */
inline uintptr_t gpaddr_to_hostaddr(uintptr_t gpaddr, RPFHState *r) {
    return (uintptr_t) r->hostptr_guest_dram + (gpaddr & 0x7FFFFFFF);
}

/* fulfill the fetch request by using the copied page in its evictedframe entry */
void rpfh_fetch_page(CPURISCVState *env, target_ulong vaddr, hwaddr *paddr_res,
    target_ulong *pte)
{
    printf("rpfh_fetch_page\n");

    // get a freeframe to fulfill the fetch request
    assert(!QTAILQ_EMPTY(&headff));
    struct freeframe *ff = QTAILQ_FIRST(&headff);
    QTAILQ_REMOVE(&headff, ff, link);

    // we need to find the evictedframe that corresponds to the vaddr that just
    // caused the remote page fault. to do so, we use the vaddr's pte to get
    // the ppn.  then, we compare with the evictedframes' ppns until we find a
    // match
    struct evictedframe *ef = NULL;
    uint64_t key_pte = *pte & 0xFFFFFFFFFC00;
    bool found = false;
    QTAILQ_FOREACH(ef, &headef, link) {
        if (key_pte == (ef->pte & 0xFFFFFFFFFC00)) {
            found = true;
            break;
        }
    }
    assert(found); // by construction, we should find it
    QTAILQ_REMOVE(&headef, ef, link);

    // compute the host address for ff->gptr
    uint64_t *frame_addr = (uint64_t *) gpaddr_to_hostaddr(ff->gptr, rpfhstate);

    // copy from evictedpage to frame_addr
    memcpy(frame_addr, ef->data, 4096);
    *paddr_res = ff->gptr;

    // update pte, preserve the pte bits but remove remote bit
    target_ulong new_pte = 0;
    new_pte = (*paddr_res >> PGSHIFT) << PTE_PPN_SHIFT;
    new_pte = new_pte | (ef->pte & 0xFF);
    printf("new_pte=%lx\n", new_pte);
    *pte = new_pte;

    // finally, add the new pte to the newframe queue
    struct newframe *nf = g_malloc(sizeof(struct newframe));
    nf->pte = new_pte;
    QTAILQ_INSERT_TAIL(&headnf, nf, link);

    g_free(ff);
    g_free(ef->data);
    g_free(ef);
}


/* evict the page, store it in an evictedframe entry */
static void rpfh_evict_page(uint64_t pte_gpaddr, RPFHState *r) {
    printf("qemu rpfh evict page\n");
    // read pte
    uint64_t *pte = (uint64_t *) gpaddr_to_hostaddr(pte_gpaddr, r);

    if ((*pte & PTE_REMOTE) != 0) {
      printf("This pte is already remote\n");
      return;
    }

    /* physical address of the frame */
    uint64_t frame_gpaddr = pte_frame(pte);

    // set pte as remote
    *pte = *pte | PTE_REMOTE;

    // simulate remote memory, save page locally
    struct evictedframe *ef = g_malloc(sizeof(struct evictedframe));
    ef->data = g_malloc(4096);

    uint64_t *frame_addr = (uint64_t *) gpaddr_to_hostaddr(frame_gpaddr, r);
    memcpy(ef->data, frame_addr, 4096);
    /* To be extra careful, we zero out the frame here.
     * This is mostly to help catch bugs. */
    memset(frame_addr, 0, 4096);
    ef->pte = *pte;
    QTAILQ_INSERT_TAIL(&headef, ef, link);
}

/* process a new page published to be used by rpfh */
static void rpfh_freepage(uint64_t pte_gpaddr, RPFHState *r) {
    printf("rpfh_freepage, pte_gpaddr=%lx\n", pte_gpaddr);

    // get the paddr from the pte, and store it in a freeframe
    uint64_t *pte = (uint64_t *) gpaddr_to_hostaddr(pte_gpaddr, r);
    uint64_t frame_gpaddr = pte_frame(pte);
    struct freeframe *ff = g_malloc(sizeof(struct freeframe));
    ff->gptr = frame_gpaddr;
    QTAILQ_INSERT_TAIL(&headff, ff, link);
}

static void rpfh_queues_write(void *opaque, hwaddr mmioaddr,
                              uint64_t value, unsigned size)
{
    RPFHState *r = opaque;
    (void) r;

    if (value == 0) {
        printf("wrote 0 to pfa mmio region\n");
        return;
    }

    if (mmioaddr == PFA_INT_FREEPAGE) {
        rpfh_freepage(value, r);
    } else if (mmioaddr == PFA_INT_EVICTPAGE) {
        rpfh_evict_page(value, r);
    } else {
        printf("wrote to an invalid pfa register\n");
    }
}

static uint64_t rpfh_queues_read(void *opaque, hwaddr addr, unsigned size)
{
    printf("read from queue mmio\n");
    unsigned int count = 0;
    struct freeframe *ff = NULL;

    if (addr == PFA_INT_FREEPAGE) {
        QTAILQ_FOREACH(ff, &headff, link) {
            count++;
        }
    } else if (addr == PFA_INT_EVICTPAGE) {
        // this is always 0 because in qemu pages are "evicted"
        // instantaneously
        return 0;
    } else if (addr == PFA_INT_NEWFRAME) {
        if(QTAILQ_EMPTY(&headnf)) {
            return 0;
        }

        // get pte of newframe's head, remove from queue and free before
        // returning
        struct newframe *nf = QTAILQ_FIRST(&headnf);
        uint64_t ptenf = nf->pte;
        QTAILQ_REMOVE(&headnf, nf, link);
        g_free(nf);
        return ptenf;
    } else {
        printf("read from invalid pfa register\n");
    }

    return count;
}

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

    QTAILQ_INIT(&headff);
    QTAILQ_INIT(&headef);
    QTAILQ_INIT(&headnf);
}
