// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include <asm/barrier.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/pci_ids.h>
#include <linux/pci_regs.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <poll.h>

#include <libvfio.h>
#include <libvfio/nvme.h>

#define PCI_CLASS_STORAGE_NVME 0x010802

#define NVME_ASQ_OFFSET        SZ_4K
#define NVME_ACQ_OFFSET        (2 * SZ_4K)
#define NVME_ID_NS_OFFSET      (3 * SZ_4K)

/*
 * The I/O queues are sized by NVME_IO_QUEUE_DEPTH and are far larger than
 * everything else, so they go last and the small fixed objects above keep
 * stable offsets. Both bases stay page aligned, which the controller requires.
 */
#define NVME_IOSQ_OFFSET       (4 * SZ_4K)
#define NVME_IOSQ_BYTES        (NVME_IO_QUEUE_DEPTH * NVME_SQE_SIZE)
#define NVME_IOCQ_OFFSET       (NVME_IOSQ_OFFSET + NVME_IOSQ_BYTES)
#define NVME_IOCQ_BYTES        (NVME_IO_QUEUE_DEPTH * NVME_CQE_SIZE)
#define NVME_REGION_BYTES      (NVME_IOCQ_OFFSET + NVME_IOCQ_BYTES)

struct nvme_state {
	/* Virtual address of the mapped PCIe BAR0 MMIO space. */
	void *bar0;

	/* Current logical block allocation offset for memcpy operations. */
	__u64 lba_offset;
	/* Starting LBA on the device for the active memcpy operation. */
	__u64 memcpy_lba;
	/* Total byte size of the active memcpy operation. */
	__u64 memcpy_size;
	/* Number of commands queued for the active memcpy operation. */
	__u64 memcpy_count;
	/* Total size of the namespace in logical blocks. */
	__u64 nsze;

	/* Doorbell register stride in bytes, derived from CAP.DSTRD. */
	__u32 db_stride;
	/* Logical block size of the namespace in bytes. */
	__u32 lba_size;
	/* Power-of-two shift for converting bytes to LBAs. */
	__u32 lba_shift;

	/* MMIO pointer to the Admin Submission Queue tail doorbell. */
	volatile __u32 *asq_db;
	/* MMIO pointer to the Admin Completion Queue head doorbell. */
	volatile __u32 *acq_db;
	/* MMIO pointer to the I/O Submission Queue tail doorbell. */
	volatile __u32 *iosq_db;
	/* MMIO pointer to the I/O Completion Queue head doorbell. */
	volatile __u32 *iocq_db;

	/* Host software tail index for the Admin Submission Queue. */
	__u16 asq_tail;
	/* Host software head index for the Admin Completion Queue. */
	__u16 acq_head;
	/* Current phase polarity bit for Admin Completion Queue entries. */
	__u16 acq_phase;
	/* Host software tail index for the I/O Submission Queue. */
	__u16 iosq_tail;
	/* Host software head index for the I/O Completion Queue. */
	__u16 iocq_head;
	/* Current phase polarity bit for I/O Completion Queue entries. */
	__u16 iocq_phase;

	/* Current phase of the memcpy operation (IDLE, WRITING, or READING). */
	int memcpy_state;
	/* Source IOVA for the in-progress memcpy operation. */
	iova_t memcpy_src;
	/* Destination IOVA for the in-progress memcpy operation. */
	iova_t memcpy_dst;
	/* Flag indicating whether the caller requested an MSI interrupt. */
	bool memcpy_interrupt;
	/* Error from the write phase drained by memcpy_start(), if any. */
	int memcpy_write_ret;

	/* Negotiated I/O queue depth, clamped to the controller's CAP.MQES. */
	__u32 io_queue_depth;

	/* Device IOVA of the Admin Submission Queue ring buffer. */
	iova_t asq_iova;
	/* Device IOVA of the Admin Completion Queue ring buffer. */
	iova_t acq_iova;
	/* Device IOVA of the I/O Submission Queue ring buffer. */
	iova_t iosq_iova;
	/* Device IOVA of the I/O Completion Queue ring buffer. */
	iova_t iocq_iova;

	/* Host virtual address of the Admin Submission Queue ring buffer. */
	void *asq_vaddr;
	/* Host virtual address of the Admin Completion Queue ring buffer. */
	void *acq_vaddr;
	/* Host virtual address of the I/O Submission Queue ring buffer. */
	void *iosq_vaddr;
	/* Host virtual address of the I/O Completion Queue ring buffer. */
	void *iocq_vaddr;
};

enum {
	NVME_STATE_IDLE,
	NVME_STATE_WRITING,
	NVME_STATE_READING,
};

static int nvme_probe(struct vfio_pci_device *device)
{
	__u32 class_code;

	class_code = vfio_pci_config_readl(device, PCI_CLASS_REVISION) >> 8;
	if (class_code == PCI_CLASS_STORAGE_NVME)
		return 0;

	return -EINVAL;
}

static void nvme_submit_admin_cmd(struct nvme_state *state,
				  struct nvme_common_command *cmd,
				  struct nvme_completion *cqe)
{
	struct nvme_common_command *asq;
	struct nvme_completion *acq, *entry;
	int timeout;
	__u16 status;

	asq = state->asq_vaddr;
	acq = state->acq_vaddr;

	memcpy(&asq[state->asq_tail], cmd, sizeof(*cmd));
	state->asq_tail = (state->asq_tail + 1) % NVME_ADMIN_QUEUE_DEPTH;

	/* Ensure the command is visible to the device before the doorbell. */
	wmb();
	*state->asq_db = state->asq_tail;

	timeout = 5000;
	entry = &acq[state->acq_head];
	while (timeout > 0) {
		status = READ_ONCE(entry->status);
		if ((status & NVME_CQE_STATUS_PHASE) == state->acq_phase)
			break;
		usleep(1000);
		timeout--;
	}
	VFIO_ASSERT_GT(timeout, 0, "Admin command timeout");

	/* Do not consume the completion before the phase bit is observed. */
	rmb();
	memcpy(cqe, entry, sizeof(*cqe));

	state->acq_head = (state->acq_head + 1) % NVME_ADMIN_QUEUE_DEPTH;
	if (state->acq_head == 0)
		state->acq_phase ^= 1;

	/* Finish reading the completion before releasing the slot. */
	wmb();
	*state->acq_db = state->acq_head;
}

static void nvme_create_cq(struct vfio_pci_device *device, iova_t cq_iova,
			   __u16 qid, __u32 size)
{
	struct nvme_state *state;
	struct nvme_common_command cmd = {};
	struct nvme_completion cqe = {};

	state = device->driver.region.vaddr;

	cmd.opcode = NVME_ADMIN_OP_CREATE_CQ;
	cmd.prp1 = cq_iova;
	cmd.cdw10 = ((size - 1) << 16) | qid;
	cmd.cdw11 = (device->driver.msi << 16) | NVME_CQ_IEN | NVME_CQ_PC;

	nvme_submit_admin_cmd(state, &cmd, &cqe);
	VFIO_ASSERT_EQ(NVME_CQE_STATUS_CODE(cqe.status), 0, "Create I/O CQ failed");
}

static void nvme_create_sq(struct vfio_pci_device *device, iova_t sq_iova,
			   __u16 qid, __u16 cqid, __u32 size)
{
	struct nvme_state *state;
	struct nvme_common_command cmd = {};
	struct nvme_completion cqe = {};

	state = device->driver.region.vaddr;

	cmd.opcode = NVME_ADMIN_OP_CREATE_SQ;
	cmd.prp1 = sq_iova;
	cmd.cdw10 = ((size - 1) << 16) | qid;
	cmd.cdw11 = (cqid << 16) | NVME_SQ_PC;

	nvme_submit_admin_cmd(state, &cmd, &cqe);
	VFIO_ASSERT_EQ(NVME_CQE_STATUS_CODE(cqe.status), 0, "Create I/O SQ failed");
}

static void nvme_set_num_queues(struct vfio_pci_device *device, __u16 nr_queues)
{
	struct nvme_common_command cmd = {};
	struct nvme_completion cqe = {};
	struct nvme_state *state;

	state = device->driver.region.vaddr;

	cmd.opcode = NVME_ADMIN_OP_SET_FEATURES;
	cmd.cdw10 = NVME_FEAT_NUM_QUEUES;
	cmd.cdw11 = ((nr_queues - 1) << 16) | (nr_queues - 1);

	nvme_submit_admin_cmd(state, &cmd, &cqe);
	VFIO_ASSERT_EQ(NVME_CQE_STATUS_CODE(cqe.status), 0, "Set Features (Number of Queues) failed");
}

static void nvme_identify_ns(struct vfio_pci_device *device, iova_t iova,
			     __u32 nsid)
{
	struct nvme_common_command cmd = {};
	struct nvme_completion cqe = {};
	struct nvme_state *state;

	state = device->driver.region.vaddr;

	cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
	cmd.nsid = nsid;
	cmd.prp1 = iova;
	cmd.cdw10 = NVME_ID_CNS_NS;

	nvme_submit_admin_cmd(state, &cmd, &cqe);
	VFIO_ASSERT_EQ(NVME_CQE_STATUS_CODE(cqe.status), 0, "Identify Namespace failed");
}

static void nvme_drain_eventfd(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int timeout_ms = 1;
	__u64 val;

	while (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
		if (read(fd, &val, sizeof(val)) < 0)
			break;
	}
}

static void nvme_init(struct vfio_pci_device *device)
{
	struct nvme_id_ns *id_ns;
	struct nvme_state *state;
	void *base_vaddr, *bar0;
	__u8 flbas, lba_shift;
	int timeout, wait_ms;
	__u32 cc, csts, aqa;
	iova_t base_iova;
	__u64 cap;

	state = device->driver.region.vaddr;
	bar0 = device->bars[0].vaddr;
	base_iova = device->driver.region.iova;
	base_vaddr = device->driver.region.vaddr;

	VFIO_ASSERT_GE(device->driver.region.size, NVME_REGION_BYTES,
		       "Driver region too small for NVME_IO_QUEUE_DEPTH\n");

	memset(base_vaddr, 0, sizeof(*state));

	cap = nvme_readq(bar0, NVME_REG_CAP);
	state->db_stride = 1 << (2 + NVME_CAP_DSTRD(cap));
	timeout = max(5000ULL, NVME_CAP_TO(cap) * 500ULL);

	/*
	 * CAP.MQES is zero based and bounds the I/O queues. Clamp rather than
	 * fail: a controller that cannot take the full depth still gives a
	 * usable, if shorter, in-flight window.
	 */
	state->io_queue_depth = min_t(__u32, NVME_IO_QUEUE_DEPTH,
				      NVME_CAP_MQES(cap) + 1);

	state->bar0 = bar0;
	state->asq_vaddr = base_vaddr + NVME_ASQ_OFFSET;
	state->asq_iova = base_iova + NVME_ASQ_OFFSET;
	state->acq_vaddr = base_vaddr + NVME_ACQ_OFFSET;
	state->acq_iova = base_iova + NVME_ACQ_OFFSET;
	state->iosq_vaddr = base_vaddr + NVME_IOSQ_OFFSET;
	state->iosq_iova = base_iova + NVME_IOSQ_OFFSET;
	state->iocq_vaddr = base_vaddr + NVME_IOCQ_OFFSET;
	state->iocq_iova = base_iova + NVME_IOCQ_OFFSET;

	memset(state->asq_vaddr, 0, NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE);
	memset(state->acq_vaddr, 0, NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE);
	memset(state->iosq_vaddr, 0, state->io_queue_depth * NVME_SQE_SIZE);
	memset(state->iocq_vaddr, 0, state->io_queue_depth * NVME_CQE_SIZE);

	vfio_pci_config_writew(device, PCI_COMMAND,
			       PCI_COMMAND_MEMORY |
			       PCI_COMMAND_MASTER |
			       PCI_COMMAND_INTX_DISABLE);

	if (device->msix_info.count > 1) {
		vfio_pci_msix_enable(device, 0, 2);
		device->driver.msi = 1;
	} else if (device->msix_info.count == 1) {
		vfio_pci_msix_enable(device, 0, 1);
		device->driver.msi = 0;
	} else if (device->msi_info.count >= 1) {
		vfio_pci_msi_enable(device, 0, 1);
		device->driver.msi = 0;
	}

	cc = nvme_readl(bar0, NVME_REG_CC);
	if (cc & NVME_CC_EN) {
		cc &= ~NVME_CC_EN;
		nvme_writel(bar0, NVME_REG_CC, cc);
	}
	wait_ms = timeout;
	while (wait_ms > 0) {
		csts = nvme_readl(bar0, NVME_REG_CSTS);
		if (!(csts & NVME_CSTS_RDY))
			break;
		usleep(1000);
		wait_ms--;
	}
	VFIO_ASSERT_GT(wait_ms, 0);

	aqa = ((NVME_ADMIN_QUEUE_DEPTH - 1) << 16) | (NVME_ADMIN_QUEUE_DEPTH - 1);
	nvme_writel(bar0, NVME_REG_AQA, aqa);
	nvme_writeq(bar0, NVME_REG_ASQ, state->asq_iova);
	nvme_writeq(bar0, NVME_REG_ACQ, state->acq_iova);

	cc = (cc & 0xff00000f) | NVME_CC_EN | NVME_CC_CSS_NVM |
	     NVME_CC_MPS(0) | NVME_CC_AMS_RR | NVME_CC_IOSQES(6) |
	     NVME_CC_IOCQES(4);
	nvme_writel(bar0, NVME_REG_CC, cc);
	wait_ms = timeout;
	while (wait_ms > 0) {
		csts = nvme_readl(bar0, NVME_REG_CSTS);
		if (csts & NVME_CSTS_RDY)
			break;
		usleep(1000);
		wait_ms--;
	}
	VFIO_ASSERT_GT(wait_ms, 0);

	state->asq_db = state->bar0 + NVME_REG_DBS;
	state->acq_db = state->bar0 + NVME_REG_DBS + state->db_stride;
	state->iosq_db = state->bar0 + NVME_REG_DBS + 2 * state->db_stride;
	state->iocq_db = state->bar0 + NVME_REG_DBS + 3 * state->db_stride;

	state->asq_tail = 0;
	state->acq_head = 0;
	state->acq_phase = 1;
	state->iosq_tail = 0;
	state->iocq_head = 0;
	state->iocq_phase = 1;

	id_ns = base_vaddr + NVME_ID_NS_OFFSET;
	memset(id_ns, 0, SZ_4K);
	nvme_identify_ns(device, base_iova + NVME_ID_NS_OFFSET, NVME_DEFAULT_NSID);

	state->nsze = id_ns->nsze;
	flbas = id_ns->flbas & NVME_NS_FLBAS_LBA_MASK;
	lba_shift = id_ns->lbaf[flbas].ds;
	if (lba_shift < NVME_DEFAULT_LBA_SHIFT) {
		state->lba_shift = NVME_DEFAULT_LBA_SHIFT;
		state->lba_size = NVME_DEFAULT_LBA_SIZE;
	} else {
		state->lba_shift = lba_shift;
		state->lba_size = 1 << lba_shift;
	}

	nvme_set_num_queues(device, 1);
	nvme_create_cq(device, state->iocq_iova, NVME_IO_QID, state->io_queue_depth);
	nvme_create_sq(device, state->iosq_iova, NVME_IO_QID, NVME_IO_QID,
		       state->io_queue_depth);

	if (device->msi_eventfds[device->driver.msi] >= 0)
		nvme_drain_eventfd(device->msi_eventfds[device->driver.msi]);

	device->driver.max_memcpy_size = max_t(__u32, SZ_4K, state->lba_size);
	device->driver.max_memcpy_count = state->io_queue_depth - 1;
}

static void nvme_remove(struct vfio_pci_device *device)
{
	struct nvme_state *state;
	int timeout;
	__u32 cc;

	state = device->driver.region.vaddr;

	cc = nvme_readl(state->bar0, NVME_REG_CC);
	cc &= ~NVME_CC_EN;
	nvme_writel(state->bar0, NVME_REG_CC, cc);

	timeout = 5000;
	while (timeout > 0) {
		__u32 csts = nvme_readl(state->bar0, NVME_REG_CSTS);
		if (!(csts & NVME_CSTS_RDY))
			break;
		usleep(1000);
		timeout--;
	}
	if (device->msix_info.count)
		vfio_pci_msix_disable(device);
	else if (device->msi_info.count)
		vfio_pci_msi_disable(device);
}

static void nvme_submit_io_cmd_async(struct nvme_state *state,
				     struct nvme_common_command *cmd)
{
	struct nvme_common_command *iosq;

	iosq = state->iosq_vaddr;

	memcpy(&iosq[state->iosq_tail], cmd, sizeof(*cmd));
	state->iosq_tail = (state->iosq_tail + 1) % state->io_queue_depth;

	/* Ensure the command is visible to the device before the doorbell. */
	wmb();
	*state->iosq_db = state->iosq_tail;
}

/*
 * Return the starting LBA for the next memcpy operation and advance the
 * offset, wrapping within the namespace capacity or 1GB window.
 */
static __u64 nvme_next_lba(struct nvme_state *state, u64 size)
{
	__u64 max_lba, lba;

	max_lba = state->nsze ? min_t(__u64, state->nsze, SZ_1G >> state->lba_shift)
			      : (SZ_1G >> state->lba_shift);
	lba = state->lba_offset;
	state->lba_offset = (state->lba_offset + (size >> state->lba_shift)) % max_lba;

	return lba;
}

static int nvme_poll_io_completion(struct nvme_state *state,
				   struct nvme_completion *cqe)
{
	struct nvme_completion *iocq, *entry;
	int timeout;
	__u16 status;

	iocq = state->iocq_vaddr;

	timeout = 5000;
	entry = &iocq[state->iocq_head];
	while (timeout > 0) {
		status = READ_ONCE(entry->status);
		if ((status & NVME_CQE_STATUS_PHASE) == state->iocq_phase)
			break;
		usleep(1000);
		timeout--;
	}
	if (timeout == 0)
		return -ETIMEDOUT;

	/* Do not consume the completion before the phase bit is observed. */
	rmb();
	memcpy(cqe, entry, sizeof(*cqe));

	state->iocq_head = (state->iocq_head + 1) % state->io_queue_depth;
	if (state->iocq_head == 0)
		state->iocq_phase ^= 1;

	/* Finish reading the completion before releasing the slot. */
	wmb();
	*state->iocq_db = state->iocq_head;
	return 0;
}

/*
 * Reap @count completions. Returns -ETIMEDOUT if the device stopped
 * completing commands, -EIO if any command completed with an error status,
 * 0 if all @count completed successfully.
 */
static int nvme_reap_io_completions(struct nvme_state *state, u64 count)
{
	struct nvme_completion cqe = {};
	u64 i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = nvme_poll_io_completion(state, &cqe);
		if (ret)
			return ret;

		if (NVME_CQE_STATUS_CODE(cqe.status) != 0)
			return -EIO;
	}

	return 0;
}

static void nvme_recover(struct vfio_pci_device *device)
{
	__u32 wait_ms, csts, aqa, cap, cc;
	struct nvme_state *state;
	__u64 timeout;

	state = device->driver.region.vaddr;

	cap = nvme_readq(state->bar0, NVME_REG_CAP);
	timeout = max(5000ULL, NVME_CAP_TO(cap) * 500ULL);

	cc = nvme_readl(state->bar0, NVME_REG_CC);
	if (cc & NVME_CC_EN) {
		cc &= ~NVME_CC_EN;
		nvme_writel(state->bar0, NVME_REG_CC, cc);
	}
	wait_ms = timeout;
	while (wait_ms > 0) {
		csts = nvme_readl(state->bar0, NVME_REG_CSTS);
		if (!(csts & NVME_CSTS_RDY))
			break;
		usleep(1000);
		wait_ms--;
	}
	VFIO_ASSERT_GT(wait_ms, 0);

	vfio_pci_device_reset(device);
	vfio_pci_config_writew(device, PCI_COMMAND,
			       PCI_COMMAND_MEMORY |
			       PCI_COMMAND_MASTER |
			       PCI_COMMAND_INTX_DISABLE);

	aqa = ((NVME_ADMIN_QUEUE_DEPTH - 1) << 16) | (NVME_ADMIN_QUEUE_DEPTH - 1);
	nvme_writel(state->bar0, NVME_REG_AQA, aqa);
	nvme_writeq(state->bar0, NVME_REG_ASQ, state->asq_iova);
	nvme_writeq(state->bar0, NVME_REG_ACQ, state->acq_iova);

	cc = (cc & 0xff00000f) | NVME_CC_EN | NVME_CC_CSS_NVM |
	     NVME_CC_MPS(0) | NVME_CC_AMS_RR | NVME_CC_IOSQES(6) |
	     NVME_CC_IOCQES(4);
	nvme_writel(state->bar0, NVME_REG_CC, cc);
	wait_ms = timeout;
	while (wait_ms > 0) {
		csts = nvme_readl(state->bar0, NVME_REG_CSTS);
		if (csts & NVME_CSTS_RDY)
			break;
		usleep(1000);
		wait_ms--;
	}
	VFIO_ASSERT_GT(wait_ms, 0);

	state->asq_tail = 0;
	state->acq_head = 0;
	state->acq_phase = 1;
	state->iosq_tail = 0;
	state->iocq_head = 0;
	state->iocq_phase = 1;
	memset(state->asq_vaddr, 0, NVME_ADMIN_QUEUE_DEPTH * NVME_SQE_SIZE);
	memset(state->acq_vaddr, 0, NVME_ADMIN_QUEUE_DEPTH * NVME_CQE_SIZE);
	memset(state->iosq_vaddr, 0, state->io_queue_depth * NVME_SQE_SIZE);
	memset(state->iocq_vaddr, 0, state->io_queue_depth * NVME_CQE_SIZE);

	nvme_set_num_queues(device, 1);
	nvme_create_cq(device, state->iocq_iova, NVME_IO_QID, state->io_queue_depth);
	nvme_create_sq(device, state->iosq_iova, NVME_IO_QID, NVME_IO_QID,
		       state->io_queue_depth);
}

static void nvme_memcpy_start(struct vfio_pci_device *device,
			      iova_t src, iova_t dst, u64 size, u64 count)
{
	struct nvme_common_command cmd = {};
	struct nvme_state *state;
	bool is_aligned;
	__u32 nlb;
	u64 i;

	state = device->driver.region.vaddr;

	VFIO_ASSERT_GE(count, 1);
	VFIO_ASSERT_LE(count, device->driver.max_memcpy_count);
	VFIO_ASSERT_GT(size, 0);
	VFIO_ASSERT_LE(size, device->driver.max_memcpy_size);
	VFIO_ASSERT_LE((src & (SZ_4K - 1)) + size, 2 * SZ_4K);
	VFIO_ASSERT_LE((dst & (SZ_4K - 1)) + size, 2 * SZ_4K);

	is_aligned = (size % state->lba_size) == 0;
	VFIO_ASSERT_TRUE(is_aligned);

	state->memcpy_src = src;
	state->memcpy_dst = dst;
	state->memcpy_size = size;
	state->memcpy_count = count;
	state->memcpy_interrupt = false;
	state->memcpy_write_ret = 0;
	state->memcpy_state = NVME_STATE_WRITING;
	state->memcpy_lba = nvme_next_lba(state, size);

	nlb = (size >> state->lba_shift) - 1;
	cmd.opcode = NVME_NVM_OP_WRITE;
	cmd.nsid = NVME_DEFAULT_NSID;
	cmd.prp1 = src;
	if ((src & (SZ_4K - 1)) + size > SZ_4K)
		cmd.prp2 = (src + SZ_4K) & ~(__u64)(SZ_4K - 1);
	cmd.cdw10 = state->memcpy_lba;
	cmd.cdw11 = state->memcpy_lba >> 32;
	cmd.cdw12 = nlb;

	/*
	 * All @count commands target the same LBA range, matching the
	 * documented "repeated memcpy from src to dst" semantics, so a single
	 * read back still yields the expected data.
	 */
	for (i = 0; i < count; i++)
		nvme_submit_io_cmd_async(state, &cmd);

	/*
	 * Drain the write phase here rather than in memcpy_wait(). The device
	 * cannot read data back before it has been staged in the backing
	 * store, so the two phases cannot overlap. Completing the writes here
	 * leaves the read phase, and only the read phase, outstanding once
	 * memcpy_start() has returned.
	 *
	 * A failure cannot be reported from this void function, so stash it
	 * and let memcpy_wait() return it.
	 */
	state->memcpy_write_ret = nvme_reap_io_completions(state, count);
	if (state->memcpy_write_ret) {
		state->memcpy_state = NVME_STATE_IDLE;
		nvme_recover(device);
		return;
	}

	if (device->msi_eventfds[device->driver.msi] >= 0)
		nvme_drain_eventfd(device->msi_eventfds[device->driver.msi]);

	state->memcpy_state = NVME_STATE_READING;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = NVME_NVM_OP_READ;
	cmd.nsid = NVME_DEFAULT_NSID;
	cmd.prp1 = dst;
	if ((dst & (SZ_4K - 1)) + size > SZ_4K)
		cmd.prp2 = (dst + SZ_4K) & ~(__u64)(SZ_4K - 1);
	cmd.cdw10 = state->memcpy_lba;
	cmd.cdw11 = state->memcpy_lba >> 32;
	cmd.cdw12 = nlb;

	/*
	 * Queue the reads but do not reap them. The submission queue holds up
	 * to io_queue_depth - 1 outstanding commands, so the device is still
	 * working through them long after memcpy_start() has returned. That is
	 * what gives a caller a window in which DMA is genuinely in flight.
	 */
	for (i = 0; i < count; i++)
		nvme_submit_io_cmd_async(state, &cmd);
}

static int nvme_memcpy_wait(struct vfio_pci_device *device)
{
	struct nvme_state *state;
	int ret;

	state = device->driver.region.vaddr;

	/*
	 * memcpy_start() drains the write phase itself and has no way to
	 * report a failure. Pick it up here; the read phase was never
	 * submitted in that case.
	 */
	if (state->memcpy_write_ret) {
		ret = state->memcpy_write_ret;
		state->memcpy_write_ret = 0;
		goto out_clear_interrupt;
	}

	VFIO_ASSERT_EQ(state->memcpy_state, NVME_STATE_READING);

	ret = nvme_reap_io_completions(state, state->memcpy_count);
	state->memcpy_state = NVME_STATE_IDLE;
	if (ret)
		nvme_recover(device);

out_clear_interrupt:
	if (!state->memcpy_interrupt && device->msi_eventfds[device->driver.msi] >= 0)
		nvme_drain_eventfd(device->msi_eventfds[device->driver.msi]);

	return ret;
}

static void nvme_send_msi(struct vfio_pci_device *device)
{
	struct nvme_state *state = device->driver.region.vaddr;
	struct nvme_common_command cmd = {};
	struct nvme_completion cqe = {};

	cmd.opcode = NVME_NVM_OP_FLUSH;
	cmd.nsid = NVME_DEFAULT_NSID;

	nvme_submit_io_cmd_async(state, &cmd);

	VFIO_ASSERT_EQ(nvme_poll_io_completion(state, &cqe), 0);
	VFIO_ASSERT_EQ(NVME_CQE_STATUS_CODE(cqe.status), 0);
}

struct vfio_pci_driver_ops nvme_ops = {
	.name = "nvme",
	.probe = nvme_probe,
	.init = nvme_init,
	.remove = nvme_remove,
	.memcpy_start = nvme_memcpy_start,
	.memcpy_wait = nvme_memcpy_wait,
	.send_msi = nvme_send_msi,
};
