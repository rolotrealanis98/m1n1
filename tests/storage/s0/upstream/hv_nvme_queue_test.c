#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/hv_nvme_queue.h"

#define BLOCKS 32

//
// The queue code translates guest-physical (IPA) addresses through hv_ipa_to_pa(), which lives
// in hv_vm.c and needs the EL2 page tables. On the host the test allocates its queues and PRP
// pages in ordinary memory, so an identity stub keeps this test self-contained. Setting
// test_ipa_translation_fails exercises the untranslatable path (doorbell must NOT be silently
// swallowed - process_io() has to fail rather than leave the guest waiting for a completion).
//
bool test_ipa_translation_fails;

uint64_t hv_ipa_to_pa(uint64_t ipa)
{
    if (test_ipa_translation_fails)
        return 0;
    return ipa; // identity: host addresses are their own "PA"
}

static uint8_t disk[BLOCKS][VNVME_LBA_SIZE];
static unsigned irq_asserts;
static unsigned irq_deasserts;
static bool cqe_published;
static struct vnvme_trace_event trace_events[16];
static unsigned trace_count;
static unsigned read_commands;
static unsigned write_commands;
static unsigned last_transfer_blocks;

static bool backend_read(void *opaque, uint64_t lba, void *buffer)
{
    (void)opaque;
    if (lba >= BLOCKS)
        return false;
    memcpy(buffer, disk[lba], VNVME_LBA_SIZE);
    return true;
}

static bool backend_write(void *opaque, uint64_t lba, const void *buffer)
{
    (void)opaque;
    if (lba >= BLOCKS)
        return false;
    memcpy(disk[lba], buffer, VNVME_LBA_SIZE);
    return true;
}

static bool backend_read_blocks(void *opaque, uint64_t lba, uint32_t blocks, void *buffer)
{
    (void)opaque;
    if (!blocks || lba >= BLOCKS || blocks > BLOCKS - lba)
        return false;
    read_commands++;
    last_transfer_blocks = blocks;
    memcpy(buffer, disk[lba], (size_t)blocks * VNVME_LBA_SIZE);
    return true;
}

static bool backend_write_blocks(void *opaque, uint64_t lba, uint32_t blocks, const void *buffer)
{
    (void)opaque;
    if (!blocks || lba >= BLOCKS || blocks > BLOCKS - lba)
        return false;
    write_commands++;
    last_transfer_blocks = blocks;
    memcpy(disk[lba], buffer, (size_t)blocks * VNVME_LBA_SIZE);
    return true;
}

static bool backend_flush(void *opaque)
{
    (void)opaque;
    return true;
}

static void backend_irq(void *opaque, bool asserted)
{
    (void)opaque;
    if (asserted) {
        assert(cqe_published);
        irq_asserts++;
    } else {
        cqe_published = false;
        irq_deasserts++;
    }
}

static void backend_publish(void *opaque)
{
    (void)opaque;
    cqe_published = true;
}

static void backend_trace(void *opaque, const struct vnvme_trace_event *event)
{
    (void)opaque;
    assert(trace_count < 16);
    trace_events[trace_count++] = *event;
}

static struct vnvme_command *put_cmd(uint8_t *sq, unsigned slot, uint8_t opcode, uint16_t cid)
{
    struct vnvme_command *cmd = (struct vnvme_command *)(sq + slot * sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->opcode = opcode;
    cmd->cid = cid;
    return cmd;
}

static uint16_t cqe_status(const uint8_t *cq, unsigned slot)
{
    const struct vnvme_completion *cqe =
        (const struct vnvme_completion *)(cq + slot * sizeof(*cqe));
    return cqe->status;
}

static struct vnvme_snapshot snapshot(const struct vnvme_ctrl *ctrl)
{
    struct vnvme_snapshot value;

    memset(&value, 0xa5, sizeof(value));
    vnvme_get_snapshot(ctrl, &value);
    return value;
}

int main(void)
{
    static uint8_t admin_sq[VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t admin_cq[VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t max_admin_sq[256 * sizeof(struct vnvme_command)]
        __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t max_admin_cq[256 * sizeof(struct vnvme_completion)]
        __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t io_sq[VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t io_cq[VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t identify[VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t write_data[2 * VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint8_t read_pages[3 * VNVME_PAGE_SIZE] __attribute__((aligned(VNVME_PAGE_SIZE)));
    static uint64_t prp_list[VNVME_PAGE_SIZE / sizeof(uint64_t)]
        __attribute__((aligned(VNVME_PAGE_SIZE)));
    const struct vnvme_backend_ops ops = {
        .read = backend_read,
        .write = backend_write,
        .read_blocks = backend_read_blocks,
        .write_blocks = backend_write_blocks,
        .flush = backend_flush,
        .publish = backend_publish,
        .irq = backend_irq,
        .trace = backend_trace,
    };
    struct vnvme_ctrl ctrl;
    struct vnvme_intx_delivery delivery = {0};

    assert(!vnvme_intx_can_inject(false, 0, false, true, 0));
    assert(!vnvme_intx_can_inject(true, 1, false, true, 0));
    assert(!vnvme_intx_can_inject(true, 0, true, true, 0));
    assert(!vnvme_intx_can_inject(true, 0, false, false, 0));
    assert(!vnvme_intx_can_inject(true, 0, false, true, -1));
    assert(vnvme_intx_can_inject(true, 0, false, true, 0));

    /*
     * One continuously asserted completion source is one notification generation.  An
     * early guest EOI must not immediately inject the same CQE again before its DPC can
     * acknowledge the CQ head.  A real deassert/reassert transition starts a new generation;
     * if that transition happens while the old LR is Active, the new generation waits for
     * the old EOI and is then deliverable.
     */
    vnvme_intx_delivery_update_line(&delivery, true);
    assert(vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));
    vnvme_intx_delivery_mark_injected(&delivery);
    assert(!vnvme_intx_delivery_can_inject(&delivery, false, 0, true, 0));
    assert(!vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));
    vnvme_intx_delivery_eoi(&delivery);
    assert(!vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));
    vnvme_intx_delivery_update_line(&delivery, false);
    vnvme_intx_delivery_update_line(&delivery, true);
    assert(vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));
    vnvme_intx_delivery_mark_injected(&delivery);
    vnvme_intx_delivery_update_line(&delivery, false);
    vnvme_intx_delivery_update_line(&delivery, true);
    assert(!vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));
    vnvme_intx_delivery_eoi(&delivery);
    assert(vnvme_intx_delivery_can_inject(&delivery, true, 0, true, 0));

    /*
     * A completion produced while another physical CPU owns the EL2 trap must wake the
     * vCPU that owns the synthetic INTx LR.  Only one host IPI is needed per pending
     * assertion; owner polling or a line deassertion opens the next kick generation.
     */
    delivery = (struct vnvme_intx_delivery){0};
    assert(!vnvme_intx_delivery_should_kick_owner(&delivery, false, 0, 4, 0));
    assert(!vnvme_intx_delivery_should_kick_owner(&delivery, true, 1, 4, 0));
    assert(!vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 0, 0));
    assert(vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 4, 0));
    assert(!vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 4, 0));
    vnvme_intx_delivery_owner_polled(&delivery);
    assert(vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 4, 0));
    vnvme_intx_delivery_update_line(&delivery, false);
    assert(vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 4, 0));
    vnvme_intx_delivery_mark_injected(&delivery);
    assert(!vnvme_intx_delivery_should_kick_owner(&delivery, true, 0, 4, 0));

    vnvme_init(&ctrl, BLOCKS, &ops, NULL);
    struct vnvme_snapshot state = snapshot(&ctrl);
    assert(state.stats.sq_doorbells == 0);
    assert(state.stats.cq_doorbells == 0);
    assert(state.stats.commands == 0);
    assert(state.stats.completions == 0);
    assert(!state.irq_asserted);
    assert(vnvme_set_admin_queue(&ctrl, (uint64_t)max_admin_sq, (uint64_t)max_admin_cq, 256, 256));
    assert(!vnvme_set_admin_queue(&ctrl, (uint64_t)max_admin_sq, (uint64_t)max_admin_cq, 257, 256));

    vnvme_init(&ctrl, BLOCKS, &ops, NULL);
    assert(vnvme_set_admin_queue(&ctrl, (uint64_t)admin_sq, (uint64_t)admin_cq, 4, 4));

    /*
     * A guest may publish a whole batch with one SQ tail write. The controller must not
     * execute that entire batch synchronously in the MMIO trap: doing so prevents the guest
     * from consuming completion interrupts and turns a busy queue into multi-second stalls.
     * Keep one completion in flight and resume the queue as each CQ head is acknowledged.
     */
    put_cmd(admin_sq, 0, 0xff, 0x401);
    put_cmd(admin_sq, 1, 0xff, 0x402);
    put_cmd(admin_sq, 2, 0xff, 0x403);
    assert(vnvme_sq_doorbell(&ctrl, 0, 3));
    state = snapshot(&ctrl);
    assert(state.stats.commands == 1);
    assert(state.stats.completions == 1);
    assert(state.queues[0].sq_head == 1);
    assert(state.queues[0].sq_tail == 3);
    assert(vnvme_cq_doorbell(&ctrl, 0, 1));
    state = snapshot(&ctrl);
    assert(state.stats.commands == 2);
    assert(state.stats.completions == 2);
    assert(state.queues[0].sq_head == 2);
    assert(vnvme_cq_doorbell(&ctrl, 0, 2));
    state = snapshot(&ctrl);
    assert(state.stats.commands == 3);
    assert(state.stats.completions == 3);
    assert(state.queues[0].sq_head == 3);
    assert(vnvme_cq_doorbell(&ctrl, 0, 3));

    /* Start the functional command tests with clean queue and trace state. */
    irq_asserts = 0;
    irq_deasserts = 0;
    cqe_published = false;
    trace_count = 0;
    vnvme_init(&ctrl, BLOCKS, &ops, NULL);
    assert(vnvme_set_admin_queue(&ctrl, (uint64_t)admin_sq, (uint64_t)admin_cq, 4, 4));

    struct vnvme_command *cmd = put_cmd(admin_sq, 0, VNVME_ADMIN_IDENTIFY, 0x101);
    cmd->prp1 = (uint64_t)identify;
    cmd->cdw10 = VNVME_IDENTIFY_CONTROLLER;
    assert(vnvme_sq_doorbell(&ctrl, 0, 1));
    state = snapshot(&ctrl);
    assert(state.stats.sq_doorbells == 1);
    assert(state.stats.cq_doorbells == 0);
    assert(state.stats.commands == 1);
    assert(state.stats.completions == 1);
    assert(state.irq_asserted);
    assert(state.queues[0].sq_head == 1);
    assert(state.queues[0].sq_tail == 1);
    assert(state.queues[0].cq_head == 0);
    assert(state.queues[0].cq_tail == 1);
    assert(((const struct vnvme_completion *)admin_cq)->cid == 0x101);
    assert(cqe_status(admin_cq, 0) == 1);
    assert(identify[77] == 5); /* MDTS: 128 KiB with a 4 KiB MPS. */
    assert(identify[512] == 0x66);
    assert(identify[513] == 0x44);
    assert(*(uint32_t *)(identify + 516) == 1);
    assert(irq_asserts == 1);
    assert(trace_count == 2);
    assert(trace_events[0].type == VNVME_TRACE_SUBMISSION);
    assert(trace_events[0].qid == 0);
    assert(trace_events[0].opcode == VNVME_ADMIN_IDENTIFY);
    assert(trace_events[0].cid == 0x101);
    assert(trace_events[0].status == VNVME_SC_SUCCESS);
    assert(trace_events[1].type == VNVME_TRACE_COMPLETION);
    assert(trace_events[1].cqid == 0);
    assert(trace_events[1].cid == 0x101);
    assert(trace_events[1].phase == 1);
    assert(trace_events[1].irq_asserted);
    assert(vnvme_cq_doorbell(&ctrl, 0, 1));
    state = snapshot(&ctrl);
    assert(state.stats.cq_doorbells == 1);
    assert(state.queues[0].cq_head == 1);
    assert(state.queues[0].cq_tail == 1);
    assert(irq_deasserts == 1);

    cmd = put_cmd(admin_sq, 1, VNVME_ADMIN_CREATE_CQ, 0x102);
    cmd->prp1 = (uint64_t)io_cq;
    cmd->cdw10 = 1 | ((4 - 1) << 16);
    cmd->cdw11 = 3; /* physically contiguous and interrupts enabled */
    assert(vnvme_sq_doorbell(&ctrl, 0, 2));
    assert((cqe_status(admin_cq, 1) >> 1) == VNVME_SC_SUCCESS);
    assert(vnvme_cq_doorbell(&ctrl, 0, 2));

    cmd = put_cmd(admin_sq, 2, VNVME_ADMIN_CREATE_SQ, 0x103);
    cmd->prp1 = (uint64_t)io_sq;
    cmd->cdw10 = 1 | ((4 - 1) << 16);
    cmd->cdw11 = 1 | (1 << 16); /* contiguous, completion queue 1 */
    assert(vnvme_sq_doorbell(&ctrl, 0, 3));
    assert((cqe_status(admin_cq, 2) >> 1) == VNVME_SC_SUCCESS);
    assert(vnvme_cq_doorbell(&ctrl, 0, 3));

    for (unsigned i = 0; i < sizeof(write_data); i++)
        write_data[i] = (uint8_t)(i * 17u + 3u);
    cmd = put_cmd(io_sq, 0, VNVME_IO_WRITE, 0x201);
    cmd->nsid = 1;
    cmd->prp1 = (uint64_t)write_data;
    cmd->prp2 = (uint64_t)(write_data + VNVME_PAGE_SIZE);
    cmd->cdw10 = 2;
    cmd->cdw12 = 1; /* two logical blocks */
    assert(vnvme_sq_doorbell(&ctrl, 1, 1));
    assert(write_commands == 1);
    assert(last_transfer_blocks == 2);
    assert(memcmp(disk[2], write_data, sizeof(write_data)) == 0);
    assert((cqe_status(io_cq, 0) >> 1) == VNVME_SC_SUCCESS);
    assert(vnvme_cq_doorbell(&ctrl, 1, 1));

    /* An unaligned PRP1 plus a PRP list exercises all PRP traversal cases. */
    memset(read_pages, 0, sizeof(read_pages));
    prp_list[0] = (uint64_t)(read_pages + VNVME_PAGE_SIZE);
    prp_list[1] = (uint64_t)(read_pages + 2 * VNVME_PAGE_SIZE);
    cmd = put_cmd(io_sq, 1, VNVME_IO_READ, 0x202);
    cmd->nsid = 1;
    cmd->prp1 = (uint64_t)(read_pages + 128);
    cmd->prp2 = (uint64_t)prp_list;
    cmd->cdw10 = 2;
    cmd->cdw12 = 1;
    assert(vnvme_sq_doorbell(&ctrl, 1, 2));
    assert(read_commands == 1);
    assert(last_transfer_blocks == 2);
    assert(memcmp(read_pages + 128, write_data, VNVME_PAGE_SIZE - 128) == 0);
    assert(memcmp(read_pages + VNVME_PAGE_SIZE, write_data + VNVME_PAGE_SIZE - 128,
                  VNVME_PAGE_SIZE) == 0);
    assert(memcmp(read_pages + 2 * VNVME_PAGE_SIZE, write_data + 2 * VNVME_PAGE_SIZE - 128, 128) ==
           0);
    assert(vnvme_cq_doorbell(&ctrl, 1, 2));

    cmd = put_cmd(io_sq, 2, VNVME_IO_READ, 0x203);
    cmd->nsid = 2;
    cmd->prp1 = (uint64_t)read_pages;
    cmd->cdw12 = 0;
    assert(vnvme_sq_doorbell(&ctrl, 1, 3));
    assert((cqe_status(io_cq, 2) >> 1) == VNVME_SC_INVALID_NAMESPACE);
    assert(vnvme_cq_doorbell(&ctrl, 1, 3));

    cmd = put_cmd(io_sq, 3, VNVME_IO_FLUSH, 0x204);
    cmd->nsid = 1;
    assert(vnvme_sq_doorbell(&ctrl, 1, 0));
    assert((cqe_status(io_cq, 3) & 1) == 1);
    assert(vnvme_cq_doorbell(&ctrl, 1, 0));

    cmd = put_cmd(io_sq, 0, VNVME_IO_FLUSH, 0x205);
    cmd->nsid = 1;
    assert(vnvme_sq_doorbell(&ctrl, 1, 1));
    assert((cqe_status(io_cq, 0) & 1) == 0); /* Phase toggles after CQ wrap. */

    /* A valid tail write with an untranslatable SQE is seen but cannot consume a command. */
    vnvme_init(&ctrl, BLOCKS, &ops, NULL);
    assert(vnvme_set_admin_queue(&ctrl, (uint64_t)admin_sq, (uint64_t)admin_cq, 4, 4));
    put_cmd(admin_sq, 0, VNVME_ADMIN_IDENTIFY, 0x301);
    test_ipa_translation_fails = true;
    assert(!vnvme_sq_doorbell(&ctrl, 0, 1));
    test_ipa_translation_fails = false;
    state = snapshot(&ctrl);
    assert(state.stats.sq_doorbells == 1);
    assert(state.stats.commands == 0);
    assert(state.stats.completions == 0);
    assert(state.queues[0].sq_head == 0);
    assert(state.queues[0].sq_tail == 1);

    puts("hv_nvme_queue_test: ok");
    return 0;
}
