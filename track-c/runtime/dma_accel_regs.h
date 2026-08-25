/* dma_accel_regs.h
 *
 * BAR0 register offsets for dma-accel v0.
 * MUST stay byte-for-byte in sync with:
 *   - dma-accel-v0-register-spec.md   (source of truth)
 *   - qemu-src/hw/misc/dma_accel.c    (device model side)
 *
 * If you change a value here, change it in both other places too.
 */

#ifndef DMA_ACCEL_REGS_H
#define DMA_ACCEL_REGS_H

#define DMA_ACCEL_PCI_VENDOR_ID   0x1234
#define DMA_ACCEL_PCI_DEVICE_ID   0xda00

#define DMA_ACCEL_DEVICE_ID_VAL   0xD3A00001u

/* Register offsets within BAR0 */
#define REG_DEVICE_ID       0x00
#define REG_VERSION         0x04
#define REG_STATUS          0x08
#define REG_CONTROL         0x0C
#define REG_OPCODE          0x10
/* 0x14 reserved */
#define REG_SRC_ADDR_LO     0x18
#define REG_SRC_ADDR_HI     0x1C
#define REG_DST_ADDR_LO     0x20
#define REG_DST_ADDR_HI     0x24
#define REG_LEN             0x28
#define REG_CMD_ID_LO       0x2C
#define REG_CMD_ID_HI       0x30
#define REG_IRQ_STATUS      0x34
#define REG_IRQ_MASK        0x38
#define REG_ERROR_CODE      0x3C

/* M6: command queue registers, per dma-accel-v0-register-spec.md §8.2 */
#define REG_SQ_BASE_LO      0x40
#define REG_SQ_BASE_HI      0x44
#define REG_SQ_SIZE         0x48
#define REG_SQ_TAIL         0x4C
#define REG_CQ_BASE_LO      0x50
#define REG_CQ_BASE_HI      0x54
#define REG_CQ_SIZE         0x58
#define REG_CQ_HEAD         0x5C
#define REG_SQ_HEAD         0x60  /* RO */
#define REG_CQ_TAIL         0x64  /* RO */

/* STATUS bits */
#define STATUS_BUSY   (1u << 0)
#define STATUS_ERROR  (1u << 1)

/* CONTROL bits (v0: only START/RESET, no IRQ_ENABLE — see spec §CONTROL) */
#define CONTROL_START (1u << 0)
#define CONTROL_RESET (1u << 1)

/* IRQ_MASK / IRQ_STATUS bits */
#define IRQ_DMA_DONE    (1u << 0)
#define IRQ_DMA_ERROR   (1u << 1)
#define IRQ_DEVICE_ERROR (1u << 2)

/* OPCODE values */
#define OPCODE_COPY        0x0
#define OPCODE_SCALE_ADD   0x1 /* out[i] = a[i]*scalar + b[i], float32 elements, M11-prep */
#define OPCODE_TILE_MATMUL 0x2 /* C_tile += A_tile @ B_tile, fixed DMA_ACCEL_TILE_DIM square
				 * tiles. Reuses src_addr/src2_addr/dst_addr as A/B/C — dst_addr
				 * is both read (current accumulator) and written (new value);
				 * len must equal DMA_ACCEL_TILE_MATMUL_BYTES exactly. */

/* Tile matmul operates on fixed-size square tiles. 32x32 float32 is chosen
 * specifically so one tile (4096 bytes) fits within the existing
 * DMA_ACCEL_BUF_SIZE per-command cap without growing that device-wide
 * constant — see the M11 compute-opcode design discussion. */
#define DMA_ACCEL_TILE_DIM 32
#define DMA_ACCEL_TILE_MATMUL_BYTES (DMA_ACCEL_TILE_DIM * DMA_ACCEL_TILE_DIM * (u32)sizeof(float))
#define OPCODE_SCALE_ADD 0x1 /* out[i] = a[i]*scalar + b[i], element-wise float32 */

enum dma_accel_error {
	DMA_ACCEL_OK            = 0x00,
	DMA_ACCEL_ERR_OPCODE    = 0x01,
	DMA_ACCEL_ERR_SRC       = 0x02,
	DMA_ACCEL_ERR_DST       = 0x03,
	DMA_ACCEL_ERR_LENGTH    = 0x04,
	DMA_ACCEL_ERR_DMA_FAULT = 0x05,
	DMA_ACCEL_ERR_BUSY      = 0x06,
	DMA_ACCEL_ERR_INTERNAL  = 0x07,
	DMA_ACCEL_ERR_ABORTED   = 0x08, /* in flight or queued when RESET hit it, M8 */
};

/*
 * M6: SQ/CQ descriptor formats, per dma-accel-v0-register-spec.md §8.1.
 * __packed so the layout matches the QEMU device model's QEMU_PACKED
 * struct byte-for-byte, regardless of compiler padding decisions on
 * either side.
 */
struct dma_accel_cmd {
	u32 opcode;
	u32 len;       /* bytes; SCALE_ADD requires len % sizeof(float) == 0 */
	u64 src_addr;  /* COPY: src. SCALE_ADD: input 'a'. */
	u64 dst_addr;  /* COPY: dst. SCALE_ADD: output. */
	u64 cmd_id;
	u64 src2_addr; /* SCALE_ADD only: input 'b'. Must be 0 for COPY. */
	float scalar;  /* SCALE_ADD only: out[i] = a[i]*scalar + b[i]. Must be 0 for COPY. */
	u32 reserved;
} __packed; /* 48 bytes (opcode 4 + len 4 + src_addr 8 + dst_addr 8 + cmd_id 8 +
             * src2_addr 8 + scalar 4 + reserved 4 = 48; the old "32 bytes" comment
             * here was already wrong before this struct grew — 4+4+8+8+8+8=40, not
             * 32 — harmless since sizeof() is what actually matters, not the
             * comment, but worth fixing while touching this struct anyway). */

struct dma_accel_completion {
	u64 cmd_id;
	u32 status;
	u32 reserved;
} __packed; /* 16 bytes */

/* M6 queue depth: a driver-side choice written into SQ_SIZE/CQ_SIZE at
 * registration time (the device doesn't hardcode or enforce a depth
 * beyond "nonzero = configured"). 16 matches what we agreed for M6. */
#define DMA_ACCEL_QUEUE_DEPTH 16

/*
 * M9: userspace ABI, per dma-accel-v0-register-spec.md §11.
 * Uses __u32/__u64 (not u32/u64) since these structs cross the
 * user/kernel boundary via ioctl()/read() — this header is included by
 * both the kernel driver and (later) userspace test/client code.
 */
#include <linux/ioctl.h>
#include <linux/types.h>

struct dma_accel_buffer_alloc {
	__u32 size;         /* in: requested size, rounded up to PAGE_SIZE */
	__u32 buffer_id;    /* out: kernel-assigned handle */
	__u64 mmap_offset;  /* out: pass as the offset argument to mmap() */
};

struct dma_accel_submit {
	__u32 opcode;
	__u32 len;
	__u32 src_buffer_id;
	__u32 src_offset;
	__u32 dst_buffer_id;
	__u32 dst_offset;
	__u64 cmd_id;        /* out: kernel-assigned, echoed back unchanged in the
			       * completion. Kernel-allocated (not caller-supplied) so
			       * concurrent submitters can never collide — see spec
			       * §11.1 decision 4. */
	/* SCALE_ADD only (input 'b'); must be left zero for COPY. Bounds are
	 * validated against the buffer the same way src/dst are — see
	 * dma_accel_ioc_submit(). */
	__u32 src2_buffer_id;
	__u32 src2_offset;
	float scalar;
};

/* Identical layout to the kernel-internal struct dma_accel_completion
 * above (§8.1) — kept as a distinct type so the uAPI surface doesn't
 * silently change if the internal one ever does. */
struct dma_accel_completion_uapi {
	__u64 cmd_id;
	__u32 status;
	__u32 reserved;
};

#define DMA_ACCEL_IOC_MAGIC 'D'
#define DMA_ACCEL_IOC_BUFFER_ALLOC _IOWR(DMA_ACCEL_IOC_MAGIC, 1, struct dma_accel_buffer_alloc)
#define DMA_ACCEL_IOC_SUBMIT       _IOWR(DMA_ACCEL_IOC_MAGIC, 2, struct dma_accel_submit)

#endif /* DMA_ACCEL_REGS_H */
