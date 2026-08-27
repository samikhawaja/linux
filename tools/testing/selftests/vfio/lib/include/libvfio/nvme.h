/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_NVME_H
#define SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_NVME_H

#include <linux/types.h>

#define NVME_REG_CAP   0x0000
#define NVME_REG_VS    0x0008
#define NVME_REG_INTMS 0x000c
#define NVME_REG_INTMC 0x0010
#define NVME_REG_CC    0x0014
#define NVME_REG_CSTS  0x001c
#define NVME_REG_AQA   0x0024
#define NVME_REG_ASQ   0x0028
#define NVME_REG_ACQ   0x0030
#define NVME_REG_DBS   0x1000

#define NVME_CAP_MQES(cap)         ((cap) & 0xffff)
#define NVME_CAP_TO(cap)           (((cap) >> 24) & 0xff)
#define NVME_CAP_DSTRD(cap)        (((cap) >> 32) & 0xf)

#define NVME_CC_EN                 0x00000001
#define NVME_CC_CSS_NVM            (0 << 4)
#define NVME_CC_MPS(mps)           ((mps) << 7)
#define NVME_CC_AMS_RR             (0 << 11)
#define NVME_CC_IOSQES(es)         ((es) << 16)
#define NVME_CC_IOCQES(es)         ((es) << 20)

#define NVME_CSTS_RDY              0x00000001

#define NVME_ADMIN_OP_CREATE_SQ    0x01
#define NVME_ADMIN_OP_CREATE_CQ    0x05
#define NVME_ADMIN_OP_IDENTIFY     0x06
#define NVME_ADMIN_OP_SET_FEATURES 0x09

#define NVME_ID_CNS_NS             0x00
#define NVME_FEAT_NUM_QUEUES       0x07

#define NVME_CQ_PC                 (1 << 0)
#define NVME_CQ_IEN                (1 << 1)
#define NVME_SQ_PC                 (1 << 0)

#define NVME_NVM_OP_FLUSH          0x00
#define NVME_NVM_OP_WRITE          0x01
#define NVME_NVM_OP_READ           0x02

#define NVME_CQE_STATUS_PHASE      (1 << 0)
#define NVME_CQE_STATUS_CODE(s)    ((s) >> 1)

#define NVME_DEFAULT_NSID          1
#define NVME_IO_QID                1
#define NVME_SQE_SIZE              64
#define NVME_CQE_SIZE              16

#define NVME_ADMIN_QUEUE_DEPTH     64

/*
 * Depth of the I/O queues. This bounds how much work can be outstanding on the
 * device at once and therefore how long a caller's in-flight window lasts: a
 * full queue is (NVME_IO_QUEUE_DEPTH - 1) commands the device still has to
 * work through after memcpy_start() has returned. Clamped at runtime to the
 * controller's CAP.MQES.
 */
#define NVME_IO_QUEUE_DEPTH        16384
#define NVME_DEFAULT_LBA_SHIFT     9
#define NVME_DEFAULT_LBA_SIZE      (1 << NVME_DEFAULT_LBA_SHIFT)
#define NVME_NS_FLBAS_LBA_MASK     0x0f

#define nvme_readl(bar0, reg)       (*(volatile __u32 *)((bar0) + (reg)))
#define nvme_writel(bar0, reg, val) (*(volatile __u32 *)((bar0) + (reg)) = (val))
#define nvme_readq(bar0, reg)       (*(volatile __u64 *)((bar0) + (reg)))
#define nvme_writeq(bar0, reg, val) (*(volatile __u64 *)((bar0) + (reg)) = (val))

struct nvme_common_command {
	__u8 opcode;
	__u8 flags;
	__u16 command_id;
	__u32 nsid;
	__u32 cdw2[2];
	__u64 metadata;
	__u64 prp1;
	__u64 prp2;
	__u32 cdw10;
	__u32 cdw11;
	__u32 cdw12;
	__u32 cdw13;
	__u32 cdw14;
	__u32 cdw15;
};

struct nvme_completion {
	__u32 result;
	__u32 rsvd;
	__u16 sq_head;
	__u16 sq_id;
	__u16 command_id;
	__u16 status;
};

struct nvme_id_ctrl {
	__u16 vid;
	__u16 ssvid;
	char sn[20];
	char mn[40];
	char fr[8];
	__u8 rsvd72[4024];
};

struct nvme_lbaf {
	__u16 ms;
	__u8 ds;
	__u8 rp;
};

struct nvme_id_ns {
	__u64 nsze;
	__u64 ncap;
	__u64 nuse;
	__u8 nsfeat;
	__u8 nlbaf;
	__u8 flbas;
	__u8 mc;
	__u8 dpc;
	__u8 dps;
	__u8 nmic;
	__u8 rescap;
	__u8 fpi;
	__u8 dlfeat;
	__u16 nawun;
	__u16 nawupf;
	__u16 nacwu;
	__u16 nabsn;
	__u16 nabo;
	__u16 nabspf;
	__u16 noiob;
	__u8 nvmcap[16];
	__u16 npwg;
	__u8 rsvd66[62];
	struct nvme_lbaf lbaf[16];
	__u8 rsvd192[192];
	__u8 vs[3712];
};

#endif /* SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_NVME_H */
