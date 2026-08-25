// SPDX-License-Identifier: GPL-2.0
/*
 * dma_accel_drv.c — M3 + M4 + M5 + M6 milestones
 *
 * M3: DMA self-test (happy path) — allocate DMA-coherent buffers, submit
 * a COPY, wait for completion, verify data moved correctly.
 *
 * M4: error-path tests exercising the QEMU device model's validation
 * code — busy-rejection, invalid LEN, invalid OPCODE.
 *
 * M5: completion detection switched from polling STATUS.BUSY in a busy
 * loop to a real IRQ (MSI preferred, INTx fallback) plus
 * wait_for_completion_timeout(). Synchronous rejections (BUSY / bad
 * OPCODE / bad LEN) are still detected by an immediate STATUS read right
 * after the triggering write — those never raise an interrupt on the
 * device side (see dma_accel.c dma_accel_start()), so there is nothing
 * to wait for in those cases.
 *
 * M6: command queue. Registers a submission ring (SQ) and completion
 * ring (CQ) once at probe time, then submits one command through the
 * queue path (write descriptor into SQ, ring SQ_TAIL doorbell) instead
 * of the legacy direct-register path, and reaps the result from CQ. The
 * M3/M4 tests above are left on the legacy path unchanged — this is a
 * deliberately separate code path, not a replacement, matching the
 * device model's dual-path design.
 *
 * M7: concurrency. The device can now run up to 4 commands at once
 * (device-internal detail, no new registers — see spec §9). The driver
 * side needs no protocol change either: submit 6 commands back-to-back,
 * drain whatever CQ has each time an IRQ wakes us (a single wakeup may
 * represent several completions once queue depth exceeds device
 * concurrency), loop until all are reaped. Elapsed-time logging per
 * command makes the two-wave completion pattern visible in dmesg as
 * evidence of genuine concurrency rather than serial processing.
 *
 * Still no char device / ioctl — that is M9's uAPI, which will build on
 * top of the queue path added here, not the legacy single-command path.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/kernel.h> /* DIV_ROUND_UP, M15 */

#include "dma_accel_regs.h"

#define DRV_NAME "dma_accel"

/* Must not exceed DMA_ACCEL_BUF_SIZE on the device side (4096). */
#define SELFTEST_LEN        256
#define IRQ_WAIT_TIMEOUT_MS  500

/*
 * M9: kernel-side completion ring, distinct from the device's own CQ
 * (§8). The ISR drains CQ into this on every interrupt; userspace
 * read()/poll() only ever sees this ring, never touches CQ indices
 * directly. Sized well above DMA_ACCEL_QUEUE_DEPTH (16) so it can never
 * actually wrap under normal operation — the device physically cannot
 * have more than QUEUE_DEPTH completions outstanding at once.
 */
#define COMP_RING_SIZE 64

/* Registered DMA buffers, for ioctl(BUFFER_ALLOC) + mmap. Fixed-size
 * table for v0 — enough for a runtime that pre-allocates a handful of
 * long-lived buffers, which is the only usage pattern that exists yet. */
#define MAX_BUFFERS 64

/*
 * M12: per-session cap on buffer slots, enforced against
 * dev->buffers[] (a single MAX_BUFFERS-slot pool shared by every
 * session). Without this, one greedy or buggy session could allocate
 * every slot and starve every other concurrently open fd — MAX_BUFFERS
 * being device-wide with no per-session limit was fine when M10.5 only
 * needed to prove ownership was tracked correctly, but a real quota is
 * what actually stops that from happening in practice.
 *
 * Value: MAX_BUFFERS / DMA_ACCEL_MAX_INFLIGHT. DMA_ACCEL_MAX_INFLIGHT
 * (4, enforced device-side — see the M7 self-test's comment) is the
 * device's own real concurrency ceiling: it can never usefully execute
 * more than 4 commands at once regardless of how many sessions are
 * open. Sizing the default quota so that exactly 4 sessions can each
 * be fully served at once (4 * 16 = 64 = MAX_BUFFERS) ties this number
 * to something the hardware actually enforces, rather than picking an
 * arbitrary fraction of 64. A 5th+ concurrent session still works —
 * it just competes for slots like any other quota-bound client, same
 * as it already competes for the device's 4 execution units.
 *
 * v0 is intentionally a single fixed default for every session, not
 * configurable per-client — no SET_QUOTA ioctl exists. Add one only
 * when a real workload needs differentiated quotas; until then this
 * would be speculative uAPI surface with no caller (same reasoning as
 * v0 having no BUFFER_FREE ioctl, per spec §11.2).
 */
#define DEFAULT_BUFFER_QUOTA (MAX_BUFFERS / 4)

/*
 * Sanity cap on a single BUFFER_ALLOC request — NOT the same thing as
 * the device's per-command LEN limit (4096, enforced independently by
 * the device itself when it processes a descriptor). A buffer can be
 * much larger than one command's transfer size; this just guards
 * against an obviously-wrong request (e.g. a stray huge size_t) eating
 * all of kernel memory before the device even gets involved.
 */
#define DMA_ACCEL_BUFFER_ALLOC_MAX (16 * 1024 * 1024)

/*
 * M10.5: cmd_id -> owning session, so the ISR knows which session's
 * ring a given completion belongs to. Deliberately a small fixed-size
 * array, not a hash table — the hardware SQ itself only has
 * DMA_ACCEL_QUEUE_DEPTH (16) slots, shared by every session, so the
 * device can never have more than 16 commands truly outstanding across
 * the whole system at once. Sized to match COMP_RING_SIZE (64) for the
 * same generous-headroom reasoning as that ring, not because anything
 * requires more than ~16 entries in practice.
 */
#define MAX_INFLIGHT_CMDS COMP_RING_SIZE

struct dma_accel_session;

struct dma_accel_cmd_ctx {
	bool valid;
	u64 cmd_id;
	struct dma_accel_session *sess;
};

struct dma_accel_buf_entry {
	void *kaddr;
	dma_addr_t dma_addr;
	size_t size;
	bool in_use;
	/* M10.5: which open() session allocated this slot. NULL when
	 * !in_use. Checked on every SUBMIT so one fd can never touch
	 * another fd's buffer (see dma_accel_ioc_submit()), and walked in
	 * reverse via dma_accel_session::owned_buffers on release() so a
	 * session only ever frees what it itself allocated. */
	struct dma_accel_session *owner;
};

struct dma_accel_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	int irq;
	struct completion irq_done;

	/* M6: queue state. sq_tail/cq_head are the driver-owned monotonic
	 * counters (mirror of what we've written to the doorbell/ack
	 * registers); index into the ring is always (counter % QUEUE_DEPTH).
	 */
	struct dma_accel_cmd *sq_buf;
	dma_addr_t sq_dma;
	struct dma_accel_completion *cq_buf;
	dma_addr_t cq_dma;
	u32 sq_tail;
	u32 cq_head;

	/*
	 * M9: becomes true only after every M3-M8 self-test has passed and
	 * the character device is about to be registered. Before that, the
	 * ISR takes the original M3-M8 behavior (just complete(&irq_done)),
	 * so none of the already-verified self-test code needs to change.
	 * After that, the ISR switches to draining CQ into comp_ring for
	 * userspace instead.
	 */
	bool uapi_active;

	/* M10.5: cmd_id -> session routing table for completions. Populated
	 * by dma_accel_ioc_submit() before the command is handed to
	 * hardware (must happen first — see the ordering comment there),
	 * consumed by the ISR to find which session's comp_ring a given
	 * completion belongs to. Replaces what used to be a single
	 * device-wide comp_ring here — see dma_accel_session::comp_ring
	 * for where completions actually land now. */
	struct dma_accel_cmd_ctx cmd_table[MAX_INFLIGHT_CMDS];
	spinlock_t cmd_lock;

	/* M15: dynamic per-session fairness quota. active_sessions is a
	 * plain headcount of open fds (inc in open(), dec in release()) —
	 * NOT protected by cmd_lock, read via atomic_read() wherever it's
	 * needed, since it's only ever used to compute an approximate quota
	 * that gets re-verified precisely under cmd_lock at the actual
	 * admission point anyway (see dma_accel_ioc_submit()). total_inflight
	 * mirrors the sum of every session's sess->inflight, maintained in
	 * lockstep at the exact same two call sites sess->inflight already
	 * is (M10.5) — a dedicated atomic rather than summing per-session
	 * counters on every check, since this needs to be read cheaply and
	 * frequently (potentially many times while a submitter is blocked
	 * waiting for admission), unlike M12's buffer quota check which only
	 * runs once per allocation and can afford a bitmap scan. quota_wq is
	 * woken whenever total_inflight decreases (a completion was routed)
	 * or active_sessions decreases (a session closed) — either event can
	 * change whether some blocked submitter's admission condition now
	 * holds. */
	atomic_t active_sessions;
	atomic_t total_inflight;
	wait_queue_head_t quota_wq;

	struct dma_accel_buf_entry buffers[MAX_BUFFERS];
	struct mutex buf_mutex;

	/* M9: sole source of cmd_id for uAPI submissions (spec §11.1 decision
	 * 4). Kernel-allocated, not caller-supplied, so concurrent P2
	 * submitters can never collide on a tag — see the M9 code review that
	 * caught this. Starts at 1 so a zero-initialized completion struct is
	 * never confusable with a real one. */
	atomic64_t next_cmd_id;

	struct miscdevice miscdev;
};

/*
 * M10.5: per-open() session state. Before this, filp->private_data
 * pointed directly at the shared dma_accel_dev, so every fd could see
 * and free every other fd's buffers (dev->buffers[] was a single global
 * table with no notion of "whose slot is this"). This struct is the fix:
 * one instance is allocated in dma_accel_chr_open() and freed in
 * dma_accel_chr_release(), giving each fd its own ownership record while
 * dev->buffers[] itself stays a single global pool (the underlying
 * hardware resource genuinely is device-wide — only the *bookkeeping* of
 * who allocated which slot needed to become per-session).
 * NOTE: this struct originally shipped without completion routing — see
 * the M10.5-session-ownership.md writeup's "known gap" section for the
 * bug that left dev->comp_ring globally shared even after buffer
 * ownership was fixed (one session's read() could silently steal
 * another session's completion, per the completion-leak probe). The
 * comp_ring/comp_head/comp_tail/comp_lock/comp_wq/inflight fields below
 * close that gap: every session now owns its completion delivery
 * end-to-end, not just its buffers.
 */
struct dma_accel_session {
	struct dma_accel_dev *dev;
	/* Bit i set == this session currently owns dev->buffers[i].
	 * Walked by dma_accel_chr_release() to free exactly (and only)
	 * this session's buffers. The authoritative per-slot check remains
	 * dev->buffers[i].owner == sess (see dma_accel_ioc_submit()); this
	 * bitmap exists so release() doesn't have to scan looking for
	 * "which slots point back at me" — it's the inverse index. */
	DECLARE_BITMAP(owned_buffers, MAX_BUFFERS);

	/* M10.5: this session's own completion queue. The ISR routes each
	 * completion here (via dev->cmd_table) instead of into a single
	 * device-wide ring, so this session's read()/poll() can only ever
	 * observe completions for commands this session itself submitted.
	 */
	struct dma_accel_completion comp_ring[COMP_RING_SIZE];
	u32 comp_head; /* consumer index, advanced by this session's read() */
	u32 comp_tail; /* producer index, advanced by the ISR */
	spinlock_t comp_lock;
	wait_queue_head_t comp_wq;

	/* M10.5: commands submitted by this session that the ISR hasn't
	 * finished routing yet. dma_accel_chr_release() waits for this to
	 * reach zero before kfree()ing the session — otherwise a completion
	 * arriving after close() (device already accepted the command,
	 * hasn't finished executing it) could dereference a freed session
	 * via dev->cmd_table[i].sess. Incremented in dma_accel_ioc_submit()
	 * before the command is handed to hardware; decremented by the ISR
	 * once it has routed (or dropped) the matching completion. */
	atomic_t inflight;
};

static inline void dma_accel_write_reg(struct dma_accel_dev *dev, u32 off, u32 val)
{
	iowrite32(val, dev->bar0 + off);
}

static inline u32 dma_accel_read_reg(struct dma_accel_dev *dev, u32 off)
{
	return ioread32(dev->bar0 + off);
}

/*
 * ISR. Before the character device is active (M3-M8 self-test phase,
 * during probe()): unchanged M5 behavior, just wake whoever's waiting on
 * irq_done. After it's active (M9): drain the device's CQ into the
 * kernel-side comp_ring and wake any poll()ers instead — see the
 * uapi_active comment on the struct.
 *
 * IRQF_SHARED-safe either way: if IRQ_STATUS reads 0, this interrupt
 * isn't ours (or was already handled), so return IRQ_NONE.
 */
static irqreturn_t dma_accel_irq_handler(int irq, void *data)
{
	struct dma_accel_dev *dev = data;
	u32 irq_status = dma_accel_read_reg(dev, REG_IRQ_STATUS);

	if (!irq_status) {
		return IRQ_NONE;
	}

	dma_accel_write_reg(dev, REG_IRQ_STATUS, irq_status);

	if (!dev->uapi_active) {
		complete(&dev->irq_done);
		return IRQ_HANDLED;
	}

	{
		u32 device_cq_tail = dma_accel_read_reg(dev, REG_CQ_TAIL);

		while (dev->cq_head != device_cq_tail) {
			u32 sidx = dev->cq_head % DMA_ACCEL_QUEUE_DEPTH;
			struct dma_accel_completion comp = dev->cq_buf[sidx];
			struct dma_accel_session *owner = NULL;
			unsigned long cmd_flags;
			int i;

			/* M10.5: route this completion to the session that
			 * submitted it, instead of into a single device-wide
			 * ring. Two locks taken one after another below
			 * (cmd_lock, then owner->comp_lock) — never nested
			 * the other way round anywhere in this file, so this
			 * ordering can't deadlock against itself. */
			spin_lock_irqsave(&dev->cmd_lock, cmd_flags);
			for (i = 0; i < MAX_INFLIGHT_CMDS; i++) {
				if (dev->cmd_table[i].valid &&
				    dev->cmd_table[i].cmd_id == comp.cmd_id) {
					owner = dev->cmd_table[i].sess;
					dev->cmd_table[i].valid = false;
					break;
				}
			}
			spin_unlock_irqrestore(&dev->cmd_lock, cmd_flags);

			if (owner) {
				unsigned long sess_flags;
				u32 ridx;

				spin_lock_irqsave(&owner->comp_lock, sess_flags);
				/* Session's own ring is sized the same as the
				 * old global one (COMP_RING_SIZE) and for the
				 * same reason should never actually wrap —
				 * drop the oldest unread entry rather than
				 * overrun the array if it somehow does. */
				if (owner->comp_tail - owner->comp_head >= COMP_RING_SIZE) {
					owner->comp_head++;
				}
				ridx = owner->comp_tail % COMP_RING_SIZE;
				owner->comp_ring[ridx] = comp;
				owner->comp_tail++;
				spin_unlock_irqrestore(&owner->comp_lock, sess_flags);

				/* Session is guaranteed alive here: release()
				 * waits for inflight==0 before kfree(), and we
				 * haven't decremented it yet. */
				atomic_dec(&owner->inflight);
				/* M15: mirrors sess->inflight exactly — see the
				 * dev struct's field comment for why this is a
				 * separate atomic rather than something summed
				 * from per-session counters on demand. Waking
				 * quota_wq here is what actually lets a
				 * submitter blocked in dma_accel_ioc_submit()'s
				 * admission wait make progress once ring space
				 * frees up. */
				atomic_dec(&dev->total_inflight);
				wake_up_interruptible(&dev->quota_wq);
				wake_up_interruptible(&owner->comp_wq);
			} else {
				/* Unknown cmd_id — the owning session already
				 * closed (release() drains inflight to 0
				 * before freeing, so this really shouldn't be
				 * reachable) or this is a genuinely unexpected
				 * completion. Loud on purpose: silently
				 * dropping a completion here would otherwise
				 * look identical to "still waiting" from
				 * userspace. dev->total_inflight is
				 * deliberately NOT decremented here: reaching
				 * this branch means no valid cmd_table entry
				 * existed for this cmd_id in the first place,
				 * so there is nothing this path was ever
				 * counted for — decrementing here would
				 * double-count against whatever legitimately
				 * cleared that entry (or, in the
				 * shouldn't-happen case, corrupt the counter
				 * for an entry that was never properly
				 * admitted). */
				dev_warn(&dev->pdev->dev,
					 "completion for unknown cmd_id=0x%llx — dropped\n",
					 (unsigned long long)comp.cmd_id);
			}

			dev->cq_head++;
		}

		dma_accel_write_reg(dev, REG_CQ_HEAD, dev->cq_head);
	}

	return IRQ_HANDLED;
}

/*
 * Write all command registers and trigger CONTROL.START. Does not wait —
 * caller decides whether/how to wait for completion. Error-path tests
 * (M4) submit a command and check STATUS immediately without waiting,
 * since synchronous rejections never fire an interrupt.
 */
static void dma_accel_submit(struct dma_accel_dev *dev, u32 opcode,
			      dma_addr_t src_dma, dma_addr_t dst_dma, u32 len)
{
	dma_accel_write_reg(dev, REG_OPCODE, opcode);
	dma_accel_write_reg(dev, REG_SRC_ADDR_LO, lower_32_bits(src_dma));
	dma_accel_write_reg(dev, REG_SRC_ADDR_HI, upper_32_bits(src_dma));
	dma_accel_write_reg(dev, REG_DST_ADDR_LO, lower_32_bits(dst_dma));
	dma_accel_write_reg(dev, REG_DST_ADDR_HI, upper_32_bits(dst_dma));
	dma_accel_write_reg(dev, REG_LEN, len);
	dma_accel_write_reg(dev, REG_CMD_ID_LO, 0xC0FFEE);
	dma_accel_write_reg(dev, REG_CMD_ID_HI, 0x0);

	/* This is the write that actually kicks off the device-side timer. */
	dma_accel_write_reg(dev, REG_CONTROL, CONTROL_START);
}

/*
 * Re-trigger START without touching the other registers. Used by the
 * BUSY-rejection test: writing SRC/DST/LEN again would overwrite the
 * live values the in-flight transfer is about to read at timer-fire
 * time (v0 has no command queue / no per-command register snapshot —
 * that gap is exactly what M6 exists to close).
 */
static void dma_accel_retrigger_start(struct dma_accel_dev *dev)
{
	dma_accel_write_reg(dev, REG_CONTROL, CONTROL_START);
}

static void dma_accel_do_reset(struct dma_accel_dev *dev)
{
	dma_accel_write_reg(dev, REG_CONTROL, CONTROL_RESET);
}

/*
 * Block on the IRQ completion until the device signals DMA_DONE/
 * DMA_ERROR, or until IRQ_WAIT_TIMEOUT_MS elapses. Returns the STATUS
 * register value read immediately after waking (or after timing out —
 * in the timeout case STATUS_BUSY will still be set, which callers use
 * to detect the timeout without a separate return path).
 */
static u32 dma_accel_wait_irq(struct dma_accel_dev *dev)
{
	unsigned long left;

	left = wait_for_completion_timeout(&dev->irq_done,
					    msecs_to_jiffies(IRQ_WAIT_TIMEOUT_MS));
	if (!left) {
		dev_err(&dev->pdev->dev,
			"IRQ wait timed out after %dms\n", IRQ_WAIT_TIMEOUT_MS);
	}

	return dma_accel_read_reg(dev, REG_STATUS);
}

/*
 * Submit one COPY command from src_dma -> dst_dma and wait for the
 * device's completion IRQ. Returns 0 on success, -ETIMEDOUT if the
 * device never signaled completion, -EIO if it finished with
 * STATUS.ERROR set.
 */
static int dma_accel_do_copy(struct dma_accel_dev *dev, dma_addr_t src_dma,
			      dma_addr_t dst_dma, u32 len)
{
	u32 status;

	reinit_completion(&dev->irq_done);
	dma_accel_submit(dev, OPCODE_COPY, src_dma, dst_dma, len);

	/*
	 * The CONTROL write above is handled synchronously by QEMU before
	 * this MMIO write returns, so STATUS already reflects whether the
	 * command was accepted (BUSY) or synchronously rejected (ERROR,
	 * no BUSY) by the time we get here — no race with the async path.
	 */
	status = dma_accel_read_reg(dev, REG_STATUS);
	if (status & STATUS_BUSY) {
		status = dma_accel_wait_irq(dev);
	}

	if (status & STATUS_BUSY) {
		dev_err(&dev->pdev->dev,
			"DMA copy timed out, STATUS=0x%08x\n", status);
		return -ETIMEDOUT;
	}

	if (status & STATUS_ERROR) {
		u32 err = dma_accel_read_reg(dev, REG_ERROR_CODE);

		dev_err(&dev->pdev->dev,
			"DMA copy failed, STATUS=0x%08x ERROR_CODE=0x%08x\n",
			status, err);
		return -EIO;
	}

	return 0;
}

/*
 * One-shot self-test run from probe(). Allocates its own DMA buffers,
 * runs a single COPY, verifies, and frees everything before returning.
 */
static int dma_accel_selftest(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	void *src_buf, *dst_buf;
	dma_addr_t src_dma, dst_dma;
	int ret;
	size_t i;

	src_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &src_dma, GFP_KERNEL);
	if (!src_buf) {
		dev_err(dma_dev, "dma_alloc_coherent(src) failed\n");
		return -ENOMEM;
	}

	dst_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &dst_dma, GFP_KERNEL);
	if (!dst_buf) {
		dev_err(dma_dev, "dma_alloc_coherent(dst) failed\n");
		dma_free_coherent(dma_dev, SELFTEST_LEN, src_buf, src_dma);
		return -ENOMEM;
	}

	for (i = 0; i < SELFTEST_LEN; i++) {
		((u8 *)src_buf)[i] = (u8)(i ^ 0xA5);
	}
	memset(dst_buf, 0, SELFTEST_LEN);

	dev_info(dma_dev,
		 "M3 self-test: src_dma=%pad dst_dma=%pad len=%u\n",
		 &src_dma, &dst_dma, (unsigned int)SELFTEST_LEN);

	ret = dma_accel_do_copy(dev, src_dma, dst_dma, SELFTEST_LEN);
	if (ret) {
		dev_err(dma_dev, "M3 self-test FAILED: do_copy returned %d\n", ret);
		goto out_free;
	}

	if (memcmp(src_buf, dst_buf, SELFTEST_LEN) != 0) {
		dev_err(dma_dev,
			"M3 self-test FAILED: destination buffer does not match "
			"source pattern (DMA completed but data is wrong)\n");
		ret = -EIO;
		goto out_free;
	}

	dev_info(dma_dev, "M3 self-test PASSED: %u bytes copied correctly via DMA (IRQ path)\n",
		 (unsigned int)SELFTEST_LEN);
	ret = 0;

out_free:
	dma_free_coherent(dma_dev, SELFTEST_LEN, dst_buf, dst_dma);
	dma_free_coherent(dma_dev, SELFTEST_LEN, src_buf, src_dma);
	return ret;
}

/*
 * M4: error-path tests exercising validation/rejection code the M3
 * happy path never touches.
 */

/* Test 1: submitting while BUSY must be rejected without disturbing the
 * in-flight transfer. The in-flight transfer's eventual completion is
 * now awaited via IRQ instead of polling. */
static int dma_accel_test_busy_rejection(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	void *src_buf, *dst_buf;
	dma_addr_t src_dma, dst_dma;
	u32 status, err;
	int ret = 0;
	size_t i;

	src_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &src_dma, GFP_KERNEL);
	dst_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &dst_dma, GFP_KERNEL);
	if (!src_buf || !dst_buf) {
		dev_err(dma_dev, "M4 busy-rejection test: alloc failed\n");
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < SELFTEST_LEN; i++) {
		((u8 *)src_buf)[i] = (u8)(i ^ 0x5A);
	}
	memset(dst_buf, 0, SELFTEST_LEN);

	reinit_completion(&dev->irq_done);

	/* Start a real transfer but do not wait — the 50ms device-side
	 * latency gives us a window to hit it with a second START. */
	dma_accel_submit(dev, OPCODE_COPY, src_dma, dst_dma, SELFTEST_LEN);

	/* Re-trigger immediately, before the first transfer's timer fires.
	 * This path never raises IRQ_STATUS on the device side, so we check
	 * STATUS directly rather than waiting. */
	dma_accel_retrigger_start(dev);

	status = dma_accel_read_reg(dev, REG_STATUS);
	err = dma_accel_read_reg(dev, REG_ERROR_CODE);

	if (!(status & STATUS_BUSY) || !(status & STATUS_ERROR) ||
	    err != DMA_ACCEL_ERR_BUSY) {
		dev_err(dma_dev,
			"M4 busy-rejection FAILED: expected BUSY|ERROR with "
			"ERR_BUSY immediately after re-trigger, got STATUS=0x%08x "
			"ERROR_CODE=0x%08x\n", status, err);
		ret = -EIO;
	} else {
		dev_info(dma_dev,
			 "M4 busy-rejection: immediate rejection confirmed "
			 "(STATUS=0x%08x ERROR_CODE=ERR_BUSY)\n", status);
	}

	/* Now wait for the ORIGINAL transfer's completion IRQ and verify it
	 * finished correctly, unaffected by the rejected second START. */
	status = dma_accel_wait_irq(dev);
	if (status & STATUS_BUSY) {
		dev_err(dma_dev, "M4 busy-rejection FAILED: original transfer never completed\n");
		ret = -ETIMEDOUT;
		goto out;
	}
	if (status & STATUS_ERROR) {
		dev_err(dma_dev, "M4 busy-rejection FAILED: original transfer ended in error\n");
		ret = -EIO;
		goto out;
	}
	if (memcmp(src_buf, dst_buf, SELFTEST_LEN) != 0) {
		dev_err(dma_dev,
			"M4 busy-rejection FAILED: original transfer's data is "
			"wrong — the rejected second START corrupted it\n");
		ret = -EIO;
		goto out;
	}

	dev_info(dma_dev,
		 "M4 busy-rejection PASSED: original transfer completed "
		 "correctly despite concurrent rejected START\n");

out:
	if (dst_buf)
		dma_free_coherent(dma_dev, SELFTEST_LEN, dst_buf, dst_dma);
	if (src_buf)
		dma_free_coherent(dma_dev, SELFTEST_LEN, src_buf, src_dma);
	return ret;
}

/* Test 2: LEN == 0 must be rejected synchronously (no timer armed, no
 * BUSY transition, no IRQ ever fires). */
static int dma_accel_test_invalid_length(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	dma_addr_t dummy_dma;
	void *dummy_buf;
	u32 status, err;
	int ret = 0;

	dummy_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &dummy_dma, GFP_KERNEL);
	if (!dummy_buf) {
		return -ENOMEM;
	}

	dma_accel_do_reset(dev);
	dma_accel_submit(dev, OPCODE_COPY, dummy_dma, dummy_dma, 0 /* invalid LEN */);

	status = dma_accel_read_reg(dev, REG_STATUS);
	err = dma_accel_read_reg(dev, REG_ERROR_CODE);

	if ((status & STATUS_BUSY) || !(status & STATUS_ERROR) ||
	    err != DMA_ACCEL_ERR_LENGTH) {
		dev_err(dma_dev,
			"M4 invalid-length FAILED: expected immediate ERROR "
			"(no BUSY) with ERR_LENGTH, got STATUS=0x%08x ERROR_CODE=0x%08x\n",
			status, err);
		ret = -EIO;
	} else {
		dev_info(dma_dev,
			 "M4 invalid-length PASSED: LEN=0 rejected synchronously "
			 "with ERR_LENGTH, no BUSY transition\n");
	}

	dma_free_coherent(dma_dev, SELFTEST_LEN, dummy_buf, dummy_dma);
	return ret;
}

/* Test 3: an unrecognized OPCODE must be rejected the same way. */
static int dma_accel_test_invalid_opcode(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	dma_addr_t dummy_dma;
	void *dummy_buf;
	u32 status, err;
	int ret = 0;

	dummy_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &dummy_dma, GFP_KERNEL);
	if (!dummy_buf) {
		return -ENOMEM;
	}

	dma_accel_do_reset(dev);
	dma_accel_submit(dev, 0xFF /* invalid OPCODE */, dummy_dma, dummy_dma, SELFTEST_LEN);

	status = dma_accel_read_reg(dev, REG_STATUS);
	err = dma_accel_read_reg(dev, REG_ERROR_CODE);

	if ((status & STATUS_BUSY) || !(status & STATUS_ERROR) ||
	    err != DMA_ACCEL_ERR_OPCODE) {
		dev_err(dma_dev,
			"M4 invalid-opcode FAILED: expected immediate ERROR "
			"(no BUSY) with ERR_OPCODE, got STATUS=0x%08x ERROR_CODE=0x%08x\n",
			status, err);
		ret = -EIO;
	} else {
		dev_info(dma_dev,
			 "M4 invalid-opcode PASSED: OPCODE=0xFF rejected synchronously "
			 "with ERR_OPCODE, no BUSY transition\n");
	}

	dma_free_coherent(dma_dev, SELFTEST_LEN, dummy_buf, dummy_dma);
	return ret;
}

static int dma_accel_error_path_tests(struct dma_accel_dev *dev)
{
	int ret;

	ret = dma_accel_test_busy_rejection(dev);
	if (ret) {
		return ret;
	}

	dma_accel_do_reset(dev);
	ret = dma_accel_test_invalid_length(dev);
	if (ret) {
		return ret;
	}

	dma_accel_do_reset(dev);
	ret = dma_accel_test_invalid_opcode(dev);
	if (ret) {
		return ret;
	}

	dma_accel_do_reset(dev);
	return 0;
}

/*
 * M6: command queue. dma_accel_queue_init() allocates SQ/CQ rings and
 * registers them with the device once (base+size registers). This must
 * run AFTER dma_accel_do_reset() (error_path_tests ends with one) since
 * RESET zeroes the device's sq_head/sq_tail/cq_head/cq_tail — registering
 * the queue before that reset would have its indices clobbered.
 */
static int dma_accel_queue_init(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	size_t sq_bytes = DMA_ACCEL_QUEUE_DEPTH * sizeof(struct dma_accel_cmd);
	size_t cq_bytes = DMA_ACCEL_QUEUE_DEPTH * sizeof(struct dma_accel_completion);

	dev->sq_buf = dma_alloc_coherent(dma_dev, sq_bytes, &dev->sq_dma, GFP_KERNEL);
	if (!dev->sq_buf) {
		dev_err(dma_dev, "M6: dma_alloc_coherent(SQ) failed\n");
		return -ENOMEM;
	}

	dev->cq_buf = dma_alloc_coherent(dma_dev, cq_bytes, &dev->cq_dma, GFP_KERNEL);
	if (!dev->cq_buf) {
		dev_err(dma_dev, "M6: dma_alloc_coherent(CQ) failed\n");
		dma_free_coherent(dma_dev, sq_bytes, dev->sq_buf, dev->sq_dma);
		dev->sq_buf = NULL;
		return -ENOMEM;
	}

	dev->sq_tail = 0;
	dev->cq_head = 0;

	dma_accel_write_reg(dev, REG_SQ_BASE_LO, lower_32_bits(dev->sq_dma));
	dma_accel_write_reg(dev, REG_SQ_BASE_HI, upper_32_bits(dev->sq_dma));
	dma_accel_write_reg(dev, REG_SQ_SIZE, DMA_ACCEL_QUEUE_DEPTH);
	dma_accel_write_reg(dev, REG_CQ_BASE_LO, lower_32_bits(dev->cq_dma));
	dma_accel_write_reg(dev, REG_CQ_BASE_HI, upper_32_bits(dev->cq_dma));
	dma_accel_write_reg(dev, REG_CQ_SIZE, DMA_ACCEL_QUEUE_DEPTH);

	dev_info(dma_dev,
		 "M6: queue registered, SQ_DMA=%pad CQ_DMA=%pad depth=%d\n",
		 &dev->sq_dma, &dev->cq_dma, DMA_ACCEL_QUEUE_DEPTH);

	return 0;
}

static void dma_accel_queue_fini(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;

	if (dev->cq_buf) {
		dma_free_coherent(dma_dev,
				   DMA_ACCEL_QUEUE_DEPTH * sizeof(struct dma_accel_completion),
				   dev->cq_buf, dev->cq_dma);
		dev->cq_buf = NULL;
	}
	if (dev->sq_buf) {
		dma_free_coherent(dma_dev,
				   DMA_ACCEL_QUEUE_DEPTH * sizeof(struct dma_accel_cmd),
				   dev->sq_buf, dev->sq_dma);
		dev->sq_buf = NULL;
	}
}

/* Write one descriptor into SQ and ring the doorbell. Does not wait. */
static void dma_accel_queue_submit_ext(struct dma_accel_dev *dev, u32 opcode,
					dma_addr_t src, dma_addr_t dst, u32 len, u64 cmd_id,
					dma_addr_t src2, float scalar)
{
	u32 idx = dev->sq_tail % DMA_ACCEL_QUEUE_DEPTH;
	struct dma_accel_cmd *slot = &dev->sq_buf[idx];

	slot->opcode = opcode;
	slot->len = len;
	slot->src_addr = src;
	slot->dst_addr = dst;
	slot->cmd_id = cmd_id;
	slot->src2_addr = src2;
	slot->scalar = scalar;
	slot->reserved = 0;

	/*
	 * dma_alloc_coherent() memory needs no explicit cache-flush on x86,
	 * but a compiler/memory barrier before the doorbell write is still
	 * good practice — it's what makes "descriptor visible before
	 * doorbell" an explicit guarantee instead of an accident of this
	 * particular architecture's coherency model.
	 */
	wmb();

	dev->sq_tail++;
	dma_accel_write_reg(dev, REG_SQ_TAIL, dev->sq_tail);
}

/* COPY-only submission path — unchanged in behavior from before
 * SCALE_ADD existed. Kept as a thin wrapper (rather than updating every
 * call site's signature) specifically so the M6-M8 self-test call sites
 * below, already verified end-to-end, don't need to change at all. */
static void dma_accel_queue_submit(struct dma_accel_dev *dev, u32 opcode,
				    dma_addr_t src, dma_addr_t dst, u32 len, u64 cmd_id)
{
	dma_accel_queue_submit_ext(dev, opcode, src, dst, len, cmd_id, 0, 0.0f);
}

/*
 * Submit one command through the queue and wait for its completion.
 * Drains ALL newly-available completions (not just ours) into the log,
 * since a single IRQ can represent more than one completion once M7
 * allows multiple in flight — M6 only ever has one outstanding, but the
 * draining loop is written to not assume that.
 */
static int dma_accel_queue_do_copy(struct dma_accel_dev *dev, dma_addr_t src,
				    dma_addr_t dst, u32 len, u64 cmd_id, u32 *status_out)
{
	u32 device_cq_tail;
	unsigned long left;
	bool found = false;

	reinit_completion(&dev->irq_done);
	dma_accel_queue_submit(dev, OPCODE_COPY, src, dst, len, cmd_id);

	left = wait_for_completion_timeout(&dev->irq_done,
					    msecs_to_jiffies(IRQ_WAIT_TIMEOUT_MS));
	if (!left) {
		dev_err(&dev->pdev->dev,
			"M6: queue completion IRQ timed out after %dms\n",
			IRQ_WAIT_TIMEOUT_MS);
		return -ETIMEDOUT;
	}

	device_cq_tail = dma_accel_read_reg(dev, REG_CQ_TAIL);
	while (dev->cq_head != device_cq_tail) {
		u32 idx = dev->cq_head % DMA_ACCEL_QUEUE_DEPTH;
		struct dma_accel_completion comp = dev->cq_buf[idx];

		dev->cq_head++;

		if (comp.cmd_id == cmd_id) {
			*status_out = comp.status;
			found = true;
		} else {
			dev_info(&dev->pdev->dev,
				 "M6: reaped unrelated completion cmd_id=0x%llx status=0x%x\n",
				 comp.cmd_id, comp.status);
		}
	}
	dma_accel_write_reg(dev, REG_CQ_HEAD, dev->cq_head);

	if (!found) {
		dev_err(&dev->pdev->dev,
			"M6: IRQ fired but no completion for cmd_id=0x%llx found in CQ\n",
			cmd_id);
		return -ENOENT;
	}

	return 0;
}

/* M6 self-test: one COPY submitted entirely through the queue path. */
static int dma_accel_queue_selftest(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	void *src_buf, *dst_buf;
	dma_addr_t src_dma, dst_dma;
	u32 status;
	int ret;
	size_t i;

	src_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &src_dma, GFP_KERNEL);
	if (!src_buf) {
		return -ENOMEM;
	}
	dst_buf = dma_alloc_coherent(dma_dev, SELFTEST_LEN, &dst_dma, GFP_KERNEL);
	if (!dst_buf) {
		dma_free_coherent(dma_dev, SELFTEST_LEN, src_buf, src_dma);
		return -ENOMEM;
	}

	for (i = 0; i < SELFTEST_LEN; i++) {
		((u8 *)src_buf)[i] = (u8)(i ^ 0x3C);
	}
	memset(dst_buf, 0, SELFTEST_LEN);

	dev_info(dma_dev, "M6 self-test: submitting via queue, cmd_id=0xFACADE\n");

	ret = dma_accel_queue_do_copy(dev, src_dma, dst_dma, SELFTEST_LEN,
				       0xFACADEULL, &status);
	if (ret) {
		dev_err(dma_dev, "M6 self-test FAILED: queue_do_copy returned %d\n", ret);
		goto out_free;
	}

	if (status != DMA_ACCEL_OK) {
		dev_err(dma_dev, "M6 self-test FAILED: completion status=0x%x\n", status);
		ret = -EIO;
		goto out_free;
	}

	if (memcmp(src_buf, dst_buf, SELFTEST_LEN) != 0) {
		dev_err(dma_dev, "M6 self-test FAILED: destination data mismatch\n");
		ret = -EIO;
		goto out_free;
	}

	dev_info(dma_dev, "M6 self-test PASSED: %u bytes copied correctly via SQ/CQ\n",
		 (unsigned int)SELFTEST_LEN);
	ret = 0;

out_free:
	dma_free_coherent(dma_dev, SELFTEST_LEN, dst_buf, dst_dma);
	dma_free_coherent(dma_dev, SELFTEST_LEN, src_buf, src_dma);
	return ret;
}

/*
 * M7: concurrency test. Submits M7_TEST_COUNT commands (deliberately
 * more than DMA_ACCEL_MAX_INFLIGHT=4 on the device side) back-to-back
 * with no waiting in between, then reaps all of them. If the device is
 * genuinely running commands concurrently, completions arrive in
 * roughly two waves ~50ms apart (first 4 dispatch immediately and
 * finish together, the remaining 2 dispatch only once a slot frees and
 * finish ~50ms later) rather than six waves 50ms apart, which is what a
 * serial implementation would produce. We don't hard-fail on the exact
 * timing shape — QEMU timer jitter under host load makes a strict
 * threshold flaky (see the M6 spurious-timeout writeup) — but we log
 * per-command elapsed time so the pattern is visible in dmesg, and we do
 * hard-verify that all commands completed successfully with correct
 * data.
 */
#define M7_TEST_COUNT 6
#define M7_TEST_LEN   128

static int dma_accel_concurrency_test(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	void *src_bufs[M7_TEST_COUNT], *dst_bufs[M7_TEST_COUNT];
	dma_addr_t src_dma[M7_TEST_COUNT], dst_dma[M7_TEST_COUNT];
	u64 cmd_ids[M7_TEST_COUNT];
	bool reaped[M7_TEST_COUNT];
	u32 reaped_status[M7_TEST_COUNT];
	s64 elapsed_ms[M7_TEST_COUNT];
	ktime_t submit_time;
	int reaped_count = 0;
	unsigned long deadline;
	int ret = 0;
	int i, j;

	memset(reaped, 0, sizeof(reaped));
	memset(src_bufs, 0, sizeof(src_bufs));
	memset(dst_bufs, 0, sizeof(dst_bufs));

	for (i = 0; i < M7_TEST_COUNT; i++) {
		src_bufs[i] = dma_alloc_coherent(dma_dev, M7_TEST_LEN, &src_dma[i], GFP_KERNEL);
		dst_bufs[i] = dma_alloc_coherent(dma_dev, M7_TEST_LEN, &dst_dma[i], GFP_KERNEL);
		if (!src_bufs[i] || !dst_bufs[i]) {
			dev_err(dma_dev, "M7: buffer alloc failed at index %d\n", i);
			ret = -ENOMEM;
			goto out_free;
		}
		/* Distinct pattern per command so cross-slot data corruption
		 * (the exact bug per-slot buffers exist to prevent) would show
		 * up as a mismatch on a specific command, not a coincidence. */
		for (j = 0; j < M7_TEST_LEN; j++) {
			((u8 *)src_bufs[i])[j] = (u8)(j ^ (0x10 + i));
		}
		memset(dst_bufs[i], 0, M7_TEST_LEN);
		cmd_ids[i] = 0x7000 + i;
	}

	dev_info(dma_dev,
		 "M7 concurrency test: submitting %d commands (MAX_INFLIGHT=4 on device)\n",
		 M7_TEST_COUNT);

	reinit_completion(&dev->irq_done);
	submit_time = ktime_get();
	for (i = 0; i < M7_TEST_COUNT; i++) {
		dma_accel_queue_submit(dev, OPCODE_COPY, src_dma[i], dst_dma[i],
					M7_TEST_LEN, cmd_ids[i]);
	}

	/* Reap in waves: each wait_for_completion wakes us for one IRQ
	 * edge, which may represent multiple completions already sitting
	 * in CQ by the time we look — drain fully each time before waiting
	 * again. Loop until all M7_TEST_COUNT are accounted for or we blow
	 * a generous overall deadline. */
	deadline = jiffies + msecs_to_jiffies(2000);
	while (reaped_count < M7_TEST_COUNT && time_before(jiffies, deadline)) {
		unsigned long left = wait_for_completion_timeout(
			&dev->irq_done, msecs_to_jiffies(IRQ_WAIT_TIMEOUT_MS));
		u32 device_cq_tail;

		if (!left) {
			break; /* no progress this wave — bail to the failure check below */
		}
		reinit_completion(&dev->irq_done);

		device_cq_tail = dma_accel_read_reg(dev, REG_CQ_TAIL);
		while (dev->cq_head != device_cq_tail) {
			u32 idx = dev->cq_head % DMA_ACCEL_QUEUE_DEPTH;
			struct dma_accel_completion comp = dev->cq_buf[idx];

			dev->cq_head++;

			for (j = 0; j < M7_TEST_COUNT; j++) {
				if (!reaped[j] && comp.cmd_id == cmd_ids[j]) {
					reaped[j] = true;
					reaped_status[j] = comp.status;
					elapsed_ms[j] = ktime_ms_delta(ktime_get(), submit_time);
					reaped_count++;
					break;
				}
			}
		}
		dma_accel_write_reg(dev, REG_CQ_HEAD, dev->cq_head);
	}

	if (reaped_count != M7_TEST_COUNT) {
		dev_err(dma_dev,
			"M7 concurrency test FAILED: only %d/%d commands reaped\n",
			reaped_count, M7_TEST_COUNT);
		ret = -ETIMEDOUT;
		goto out_free;
	}

	for (i = 0; i < M7_TEST_COUNT; i++) {
		if (reaped_status[i] != DMA_ACCEL_OK) {
			dev_err(dma_dev, "M7: cmd_id=0x%llx completed with status=0x%x\n",
				cmd_ids[i], reaped_status[i]);
			ret = -EIO;
			continue;
		}
		if (memcmp(src_bufs[i], dst_bufs[i], M7_TEST_LEN) != 0) {
			dev_err(dma_dev, "M7: cmd_id=0x%llx data mismatch\n", cmd_ids[i]);
			ret = -EIO;
			continue;
		}
		dev_info(dma_dev, "M7: cmd_id=0x%llx completed OK, elapsed=%lldms\n",
			 cmd_ids[i], elapsed_ms[i]);
	}

	if (ret == 0) {
		dev_info(dma_dev,
			 "M7 concurrency test PASSED: all %d commands correct "
			 "(see elapsed times above — two waves ~50ms apart "
			 "indicate genuine concurrency, not serial execution)\n",
			 M7_TEST_COUNT);
	}

out_free:
	for (i = 0; i < M7_TEST_COUNT; i++) {
		if (dst_bufs[i]) {
			dma_free_coherent(dma_dev, M7_TEST_LEN, dst_bufs[i], dst_dma[i]);
		}
		if (src_bufs[i]) {
			dma_free_coherent(dma_dev, M7_TEST_LEN, src_bufs[i], src_dma[i]);
		}
	}
	return ret;
}

/*
 * M8: reset/recovery test. Submits M8_TEST_COUNT commands (enough to
 * span both "in a slot" and "still queued" per M7's dispatch behavior),
 * fires CONTROL.RESET immediately — before the 50ms DMA latency could
 * possibly have elapsed — and verifies:
 *
 *  1. All commands get reaped via the normal IRQ+CQ path (not a
 *     wait_for_completion timeout — that would mean the ABORTED-signal
 *     guarantee from spec §10.1 isn't actually working).
 *  2. Every one of them carries status == DMA_ACCEL_ERR_ABORTED.
 *  3. Destination buffers are still all-zero — proof RESET genuinely
 *     preempted execution rather than letting the DMA finish first and
 *     merely mislabeling a successful copy as aborted.
 *  4. The device is fully usable afterward: one more ordinary command
 *     submitted post-reset must complete normally with correct data.
 */
#define M8_TEST_COUNT 6
#define M8_TEST_LEN   128

static int dma_accel_reset_test(struct dma_accel_dev *dev)
{
	struct device *dma_dev = &dev->pdev->dev;
	void *src_bufs[M8_TEST_COUNT], *dst_bufs[M8_TEST_COUNT];
	dma_addr_t src_dma[M8_TEST_COUNT], dst_dma[M8_TEST_COUNT];
	u64 cmd_ids[M8_TEST_COUNT];
	bool reaped[M8_TEST_COUNT];
	u32 reaped_status[M8_TEST_COUNT];
	int reaped_count = 0;
	unsigned long deadline;
	int ret = 0;
	int i, j;

	memset(reaped, 0, sizeof(reaped));
	memset(src_bufs, 0, sizeof(src_bufs));
	memset(dst_bufs, 0, sizeof(dst_bufs));

	for (i = 0; i < M8_TEST_COUNT; i++) {
		src_bufs[i] = dma_alloc_coherent(dma_dev, M8_TEST_LEN, &src_dma[i], GFP_KERNEL);
		dst_bufs[i] = dma_alloc_coherent(dma_dev, M8_TEST_LEN, &dst_dma[i], GFP_KERNEL);
		if (!src_bufs[i] || !dst_bufs[i]) {
			dev_err(dma_dev, "M8: buffer alloc failed at index %d\n", i);
			ret = -ENOMEM;
			goto out_free;
		}
		for (j = 0; j < M8_TEST_LEN; j++) {
			((u8 *)src_bufs[i])[j] = (u8)(j ^ (0x30 + i));
		}
		memset(dst_bufs[i], 0, M8_TEST_LEN);
		cmd_ids[i] = 0x8000 + i;
	}

	dev_info(dma_dev,
		 "M8 reset test: submitting %d commands, then resetting immediately\n",
		 M8_TEST_COUNT);

	reinit_completion(&dev->irq_done);
	for (i = 0; i < M8_TEST_COUNT; i++) {
		dma_accel_queue_submit(dev, OPCODE_COPY, src_dma[i], dst_dma[i],
					M8_TEST_LEN, cmd_ids[i]);
	}

	/* No delay here on purpose — RESET must land well before the 50ms
	 * device latency, or this test would only prove RESET works on
	 * already-finished commands, which is the uninteresting case. */
	dma_accel_do_reset(dev);

	deadline = jiffies + msecs_to_jiffies(2000);
	while (reaped_count < M8_TEST_COUNT && time_before(jiffies, deadline)) {
		unsigned long left = wait_for_completion_timeout(
			&dev->irq_done, msecs_to_jiffies(IRQ_WAIT_TIMEOUT_MS));
		u32 device_cq_tail;

		if (!left) {
			break;
		}
		reinit_completion(&dev->irq_done);

		device_cq_tail = dma_accel_read_reg(dev, REG_CQ_TAIL);
		while (dev->cq_head != device_cq_tail) {
			u32 idx = dev->cq_head % DMA_ACCEL_QUEUE_DEPTH;
			struct dma_accel_completion comp = dev->cq_buf[idx];

			dev->cq_head++;

			for (j = 0; j < M8_TEST_COUNT; j++) {
				if (!reaped[j] && comp.cmd_id == cmd_ids[j]) {
					reaped[j] = true;
					reaped_status[j] = comp.status;
					reaped_count++;
					break;
				}
			}
		}
		dma_accel_write_reg(dev, REG_CQ_HEAD, dev->cq_head);
	}

	if (reaped_count != M8_TEST_COUNT) {
		dev_err(dma_dev,
			"M8 reset test FAILED: only %d/%d commands reaped via IRQ+CQ "
			"(RESET's ABORTED-completion guarantee did not hold — driver "
			"would have had to fall back to a timeout for the rest)\n",
			reaped_count, M8_TEST_COUNT);
		ret = -ETIMEDOUT;
		goto out_free;
	}

	for (i = 0; i < M8_TEST_COUNT; i++) {
		bool dst_is_zero = true;

		for (j = 0; j < M8_TEST_LEN; j++) {
			if (((u8 *)dst_bufs[i])[j] != 0) {
				dst_is_zero = false;
				break;
			}
		}

		if (reaped_status[i] != DMA_ACCEL_ERR_ABORTED) {
			dev_err(dma_dev,
				"M8: cmd_id=0x%llx expected ABORTED, got status=0x%x\n",
				cmd_ids[i], reaped_status[i]);
			ret = -EIO;
			continue;
		}
		if (!dst_is_zero) {
			dev_err(dma_dev,
				"M8: cmd_id=0x%llx destination buffer is non-zero — "
				"RESET did not actually preempt this DMA\n",
				cmd_ids[i]);
			ret = -EIO;
			continue;
		}
		dev_info(dma_dev,
			 "M8: cmd_id=0x%llx correctly ABORTED, destination untouched\n",
			 cmd_ids[i]);
	}

	if (ret) {
		goto out_free;
	}

	/* Recovery check: device must be fully usable after reset. */
	{
		void *rsrc, *rdst;
		dma_addr_t rsrc_dma, rdst_dma;
		u32 rstatus;

		rsrc = dma_alloc_coherent(dma_dev, M8_TEST_LEN, &rsrc_dma, GFP_KERNEL);
		rdst = dma_alloc_coherent(dma_dev, M8_TEST_LEN, &rdst_dma, GFP_KERNEL);
		if (!rsrc || !rdst) {
			dev_err(dma_dev, "M8 recovery check: alloc failed\n");
			ret = -ENOMEM;
			if (rsrc) dma_free_coherent(dma_dev, M8_TEST_LEN, rsrc, rsrc_dma);
			if (rdst) dma_free_coherent(dma_dev, M8_TEST_LEN, rdst, rdst_dma);
			goto out_free;
		}
		for (j = 0; j < M8_TEST_LEN; j++) {
			((u8 *)rsrc)[j] = (u8)(j ^ 0x99);
		}
		memset(rdst, 0, M8_TEST_LEN);

		ret = dma_accel_queue_do_copy(dev, rsrc_dma, rdst_dma, M8_TEST_LEN,
					       0x8FFFULL, &rstatus);
		if (!ret && rstatus == DMA_ACCEL_OK &&
		    memcmp(rsrc, rdst, M8_TEST_LEN) == 0) {
			dev_info(dma_dev,
				 "M8 recovery check PASSED: device fully usable after reset\n");
		} else {
			dev_err(dma_dev,
				"M8 recovery check FAILED: ret=%d status=0x%x\n",
				ret, rstatus);
			ret = ret ? ret : -EIO;
		}

		dma_free_coherent(dma_dev, M8_TEST_LEN, rdst, rdst_dma);
		dma_free_coherent(dma_dev, M8_TEST_LEN, rsrc, rsrc_dma);
	}

	if (ret == 0) {
		dev_info(dma_dev, "M8 reset test PASSED\n");
	}

out_free:
	for (i = 0; i < M8_TEST_COUNT; i++) {
		if (dst_bufs[i]) {
			dma_free_coherent(dma_dev, M8_TEST_LEN, dst_bufs[i], dst_dma[i]);
		}
		if (src_bufs[i]) {
			dma_free_coherent(dma_dev, M8_TEST_LEN, src_bufs[i], src_dma[i]);
		}
	}
	return ret;
}

/* ============================================================
 * M9: userspace ABI — character device (buffer alloc, submit,
 * completion read/poll). See dma-accel-v0-register-spec.md §11.
 * ============================================================ */

static struct dma_accel_dev *miscdev_to_dma_accel(struct miscdevice *m)
{
	return container_of(m, struct dma_accel_dev, miscdev);
}

static int dma_accel_chr_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *m = filp->private_data;
	struct dma_accel_session *sess;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess) {
		return -ENOMEM;
	}
	sess->dev = miscdev_to_dma_accel(m);
	/* owned_buffers starts zeroed by kzalloc — no buffers owned yet. */
	spin_lock_init(&sess->comp_lock);
	init_waitqueue_head(&sess->comp_wq);
	/* comp_head/comp_tail/inflight all start at 0 — kzalloc'd. */

	/* M15: this session now counts toward the fairness-quota divisor
	 * for every OTHER open session too — see admission logic in
	 * dma_accel_ioc_submit(). No wake needed here: a new session only
	 * ever shrinks others' quota, never grows it, so it can't newly
	 * satisfy anyone currently blocked waiting for admission. */
	atomic_inc(&sess->dev->active_sessions);

	filp->private_data = sess;
	return 0;
}

static int dma_accel_chr_release(struct inode *inode, struct file *filp)
{
	struct dma_accel_session *sess = filp->private_data;
	struct dma_accel_dev *dev = sess->dev;
	int i;

	/* M15: this session stops competing for admission from this point
	 * forward (release() never calls submit()) — excluding it from
	 * active_sessions immediately, rather than after the inflight-drain
	 * wait below, makes every OTHER open session's fairness quota
	 * accurate sooner rather than staying artificially tight while this
	 * session is merely tearing down. Waking quota_wq after the
	 * decrement lets any submitter currently blocked on admission
	 * re-check with the new (larger) quota. This is safe regardless of
	 * how long the inflight-drain below takes: the actual hard cap
	 * (dev->total_inflight < DMA_ACCEL_QUEUE_DEPTH, M14) is completely
	 * independent of active_sessions and still fully protects the
	 * physical ring even while this session's own commands are still
	 * draining. */
	atomic_dec(&dev->active_sessions);
	wake_up_interruptible(&dev->quota_wq);

	/* M10.5: don't free this session while a command it submitted might
	 * still complete — dev->cmd_table[i].sess is a live pointer to it,
	 * dereferenced by the ISR possibly on another CPU. Wait for every
	 * outstanding command this session submitted to actually be routed
	 * (the ISR decrements inflight as each one is delivered or dropped,
	 * see dma_accel_irq_handler()) before touching anything below.
	 * MVP: a short polling sleep — release() runs in ordinary process
	 * context (never atomic), and the hardware SQ is bounded at
	 * DMA_ACCEL_QUEUE_DEPTH=16 total across every session (enforced
	 * kernel-side as of M14, not just by P2's Stream backpressure), so
	 * this is a very short wait in practice, not a source of real
	 * latency. Revisit only if that stops being true. */
	while (atomic_read(&sess->inflight) > 0) {
		msleep(1);
	}

	/* M10.5: buffer lifetime is tied to the fd — free exactly what
	 * *this* open() session allocated, per spec §11.2. Walking
	 * owned_buffers (not "every in_use slot", as before the fix) is
	 * what makes this session-scoped instead of global: two
	 * concurrently open fds no longer step on each other's buffers
	 * when one of them closes. */
	mutex_lock(&dev->buf_mutex);
	for_each_set_bit(i, sess->owned_buffers, MAX_BUFFERS) {
		dma_free_coherent(&dev->pdev->dev, dev->buffers[i].size,
				   dev->buffers[i].kaddr, dev->buffers[i].dma_addr);
		dev->buffers[i].in_use = false;
		dev->buffers[i].owner = NULL;
	}
	mutex_unlock(&dev->buf_mutex);

	kfree(sess);
	return 0;
}

static long dma_accel_ioc_buffer_alloc(struct dma_accel_session *sess, unsigned long arg)
{
	struct dma_accel_dev *dev = sess->dev;
	struct dma_accel_buffer_alloc req;
	int slot = -1;
	int i;
	void *kaddr;
	dma_addr_t dma_addr;
	size_t size;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
		return -EFAULT;
	}
	if (req.size == 0 || req.size > DMA_ACCEL_BUFFER_ALLOC_MAX) {
		return -EINVAL;
	}
	size = PAGE_ALIGN(req.size);

	mutex_lock(&dev->buf_mutex);

	/* M12: quota check comes before the free-slot scan, not after —
	 * if this session is already at its cap, there's no point spending
	 * a scan (and definitely no point calling dma_alloc_coherent()
	 * below) just to find out. bitmap_weight() over owned_buffers is
	 * this session's own count, distinct from "is the whole device
	 * full" below — a different errno (EDQUOT vs ENOSPC) so a caller
	 * can tell "you personally are over your share" apart from "there
	 * is genuinely nothing left for anyone." */
	if (bitmap_weight(sess->owned_buffers, MAX_BUFFERS) >= DEFAULT_BUFFER_QUOTA) {
		mutex_unlock(&dev->buf_mutex);
		return -EDQUOT;
	}

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!dev->buffers[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_unlock(&dev->buf_mutex);
		return -ENOSPC;
	}

	kaddr = dma_alloc_coherent(&dev->pdev->dev, size, &dma_addr, GFP_KERNEL);
	if (!kaddr) {
		mutex_unlock(&dev->buf_mutex);
		return -ENOMEM;
	}

	dev->buffers[slot].kaddr = kaddr;
	dev->buffers[slot].dma_addr = dma_addr;
	dev->buffers[slot].size = size;
	dev->buffers[slot].in_use = true;
	/* M10.5: record ownership both directions — the slot points back
	 * at the owning session (checked on every SUBMIT), and the
	 * session's bitmap records the slot (walked on release()). */
	dev->buffers[slot].owner = sess;
	set_bit(slot, sess->owned_buffers);
	mutex_unlock(&dev->buf_mutex);

	req.buffer_id = slot;
	/* mmap_offset encodes the buffer_id as a page-based offset, the
	 * standard V4L2/DRM convention for "fake" mmap offsets that don't
	 * correspond to real file positions. .mmap() below decodes it back
	 * via vma->vm_pgoff, which the kernel already right-shifts by
	 * PAGE_SHIFT for us. */
	req.mmap_offset = (__u64)slot << PAGE_SHIFT;

	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		/* Don't leak the buffer we just allocated. */
		mutex_lock(&dev->buf_mutex);
		dma_free_coherent(&dev->pdev->dev, size, kaddr, dma_addr);
		dev->buffers[slot].in_use = false;
		dev->buffers[slot].owner = NULL;
		clear_bit(slot, sess->owned_buffers);
		mutex_unlock(&dev->buf_mutex);
		return -EFAULT;
	}

	return 0;
}

/*
 * Whether a given opcode consumes the req.src2_buffer_id/src2_offset
 * fields of struct dma_accel_submit. Deliberately a switch over the
 * opcodes that DO need src2, not a negative check against the ones that
 * don't (e.g. "opcode != OPCODE_COPY") — a negative check silently
 * covers every future opcode by default, which is exactly how
 * OPCODE_TILE_MATMUL fell through uninitialized (src2_dma stayed 0)
 * when this used to be a single "if (opcode == OPCODE_SCALE_ADD)"
 * check: TILE_MATMUL was neither COPY nor SCALE_ADD, so it silently
 * matched neither branch. Every new opcode that needs src2 must be
 * added to this switch explicitly — there is no default that gets it
 * right for free.
 */
static bool dma_accel_opcode_needs_src2(u32 opcode)
{
	switch (opcode) {
	case OPCODE_SCALE_ADD:
	case OPCODE_TILE_MATMUL:
		return true;
	default:
		return false;
	}
}

/*
 * M15: lock-free, best-effort read of whether this session's admission
 * would currently be allowed — both the physical ring's hard cap (M14)
 * and this session's dynamic fair-share cap (M15) must hold. Used only
 * as wait_event_interruptible()'s wake-up predicate in
 * dma_accel_ioc_submit(): a stale/racy read here can only cause an
 * extra harmless wakeup-and-recheck (the real decision always happens
 * under dev->cmd_lock, see below), never an incorrect admission.
 *
 * Quota is DIV_ROUND_UP(DMA_ACCEL_QUEUE_DEPTH, active_sessions), not a
 * fixed number: with exactly one session open this evaluates to 16 —
 * the full ring, identical to what M10's Stream::throttle_before_submit()
 * already assumes — so a single client's throughput is completely
 * unaffected by this milestone. The quota only shrinks as more sessions
 * genuinely compete for the device.
 */
static bool dma_accel_admission_likely_ok(struct dma_accel_session *sess)
{
	struct dma_accel_dev *dev = sess->dev;
	int active = atomic_read(&dev->active_sessions);
	int quota = DIV_ROUND_UP(DMA_ACCEL_QUEUE_DEPTH, active < 1 ? 1 : active);

	return atomic_read(&dev->total_inflight) < DMA_ACCEL_QUEUE_DEPTH &&
	       atomic_read(&sess->inflight) < quota;
}

static long dma_accel_ioc_submit(struct dma_accel_session *sess, unsigned long arg)
{
	struct dma_accel_dev *dev = sess->dev;
	struct dma_accel_submit req;
	dma_addr_t src_dma, dst_dma, src2_dma = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
		return -EFAULT;
	}

	mutex_lock(&dev->buf_mutex);

	if (req.src_buffer_id >= MAX_BUFFERS || !dev->buffers[req.src_buffer_id].in_use ||
	    req.dst_buffer_id >= MAX_BUFFERS || !dev->buffers[req.dst_buffer_id].in_use) {
		mutex_unlock(&dev->buf_mutex);
		return -EINVAL;
	}

	/* M10.5: ownership check. Being in_use is not enough — that only
	 * proves *some* session allocated this slot, not that it was
	 * *this* one. Without this, any fd could submit a command against
	 * any other fd's buffer_id (this is the actual isolation bug the
	 * per-session ownership fields exist to close). Deliberately
	 * -EPERM, distinct from -EINVAL above, so a caller/test can tell
	 * "bad handle" apart from "handle valid but not yours". */
	if (dev->buffers[req.src_buffer_id].owner != sess ||
	    dev->buffers[req.dst_buffer_id].owner != sess) {
		mutex_unlock(&dev->buf_mutex);
		return -EPERM;
	}

	if ((u64)req.src_offset + req.len > dev->buffers[req.src_buffer_id].size ||
	    (u64)req.dst_offset + req.len > dev->buffers[req.dst_buffer_id].size) {
		mutex_unlock(&dev->buf_mutex);
		return -EINVAL; /* out-of-bounds — never reaches the device, per spec §11.3 */
	}

	/*
	 * Opcodes that need a second, independently-validated input buffer
	 * go through dma_accel_opcode_needs_src2() rather than an inline
	 * opcode comparison — see the comment on that function for why.
	 * Gated at all (vs. unconditionally resolving src2_buffer_id): for
	 * an opcode that doesn't use it, that field is caller-supplied and
	 * carries no promised meaning (per the uAPI comment, expected to be
	 * left at 0 — which is itself a valid buffer_id, not a sentinel for
	 * "unused"). Validating/dereferencing dev->buffers[] with it for an
	 * opcode that was never going to use it would be checking a field
	 * the caller never claimed was meaningful.
	 */
	if (dma_accel_opcode_needs_src2(req.opcode)) {
		if (req.src2_buffer_id >= MAX_BUFFERS ||
		    !dev->buffers[req.src2_buffer_id].in_use) {
			mutex_unlock(&dev->buf_mutex);
			return -EINVAL;
		}
		/* M10.5: same ownership check as src/dst above — src2 is a
		 * third independently-supplied buffer_id and needs the same
		 * scrutiny, not just an in_use check. */
		if (dev->buffers[req.src2_buffer_id].owner != sess) {
			mutex_unlock(&dev->buf_mutex);
			return -EPERM;
		}
		if ((u64)req.src2_offset + req.len > dev->buffers[req.src2_buffer_id].size) {
			mutex_unlock(&dev->buf_mutex);
			return -EINVAL;
		}
		src2_dma = dev->buffers[req.src2_buffer_id].dma_addr + req.src2_offset;
	}

	src_dma = dev->buffers[req.src_buffer_id].dma_addr + req.src_offset;
	dst_dma = dev->buffers[req.dst_buffer_id].dma_addr + req.dst_offset;

	mutex_unlock(&dev->buf_mutex);

	/* Kernel is the sole allocator of cmd_id (spec §11.1 decision 4) —
	 * any caller-supplied value in req.cmd_id is ignored. atomic64 keeps
	 * this collision-free even once multiple threads/processes are
	 * submitting concurrently, which a userspace-side counter could not
	 * guarantee. */
	req.cmd_id = (u64)atomic64_inc_return(&dev->next_cmd_id);

	/* M10.5: register cmd_id -> session BEFORE the command is handed to
	 * hardware. Ordering is load-bearing: if this happened after
	 * dma_accel_queue_submit_ext() below, the device could in principle
	 * complete the command and fire an interrupt before the table entry
	 * exists, and the ISR would have no session to route the completion
	 * to. atomic_inc(&sess->inflight) is part of the same
	 * before-hardware-sees-it guarantee — dma_accel_chr_release() relies
	 * on inflight to know when it's safe to free this session.
	 *
	 * M15: this registration now BLOCKS (does not return an error)
	 * until both the physical ring's hard cap (M14) and this session's
	 * dynamic fair-share quota allow it — see
	 * dma_accel_admission_likely_ok()'s comment for why the quota is
	 * ceil(16/active_sessions) rather than a fixed number, and why
	 * blocking (not returning EDQUOT) is what lets Stream::submit()
	 * need zero code changes: from userspace's perspective this ioctl
	 * just occasionally takes a bit longer, exactly as if the device
	 * itself were briefly slower. The loop below is the standard
	 * "check under lock, sleep without the lock, recheck under the
	 * lock on wakeup" pattern — dma_accel_admission_likely_ok() is
	 * only ever a hint for when to re-check; the actual decision (and
	 * the table registration itself) always happens under
	 * dev->cmd_lock, so a stale/racy wakeup just costs one extra
	 * lock+recheck, never an incorrect admission. Interruptible so a
	 * caller blocked here can still be signaled (e.g. Ctrl-C) rather
	 * than hanging unkillably. */
	{
		unsigned long cmd_flags;
		int slot;

		for (;;) {
			int active = atomic_read(&dev->active_sessions);
			int quota = DIV_ROUND_UP(DMA_ACCEL_QUEUE_DEPTH, active < 1 ? 1 : active);
			int j;
			int ret;

			spin_lock_irqsave(&dev->cmd_lock, cmd_flags);

			if (atomic_read(&dev->total_inflight) < DMA_ACCEL_QUEUE_DEPTH &&
			    atomic_read(&sess->inflight) < quota) {
				slot = -1;
				for (j = 0; j < MAX_INFLIGHT_CMDS; j++) {
					if (!dev->cmd_table[j].valid) {
						slot = j;
						break;
					}
				}
				if (slot >= 0) {
					dev->cmd_table[slot].valid = true;
					dev->cmd_table[slot].cmd_id = req.cmd_id;
					dev->cmd_table[slot].sess = sess;
					spin_unlock_irqrestore(&dev->cmd_lock, cmd_flags);
					break; /* admitted */
				}
				/* Unreachable in practice — total_inflight <
				 * 16 < MAX_INFLIGHT_CMDS (64) already
				 * guarantees a free table slot exists. Falls
				 * through to the wait below as a defensive
				 * fallback, same reasoning as M14's. */
			}

			spin_unlock_irqrestore(&dev->cmd_lock, cmd_flags);

			ret = wait_event_interruptible(dev->quota_wq,
							 dma_accel_admission_likely_ok(sess));
			if (ret) {
				return ret; /* -ERESTARTSYS on signal */
			}
			/* Wake was just a hint — loop back and re-verify
			 * precisely under the lock before admitting anything. */
		}

		atomic_inc(&sess->inflight);
		atomic_inc(&dev->total_inflight);
	}

	/* Reuses the exact same enqueue path M6-M8 already validated, now
	 * via the _ext() variant that also carries src2/scalar (harmless
	 * zeros for COPY, per dma_accel_ioc_submit()'s src2_dma init above). */
	dma_accel_queue_submit_ext(dev, req.opcode, src_dma, dst_dma, req.len, req.cmd_id,
				    src2_dma, req.scalar);

	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		/* The command is already enqueued at this point — we can't
		 * un-submit it. The caller loses the ability to match this
		 * one completion by cmd_id, but it will still show up on
		 * read()/poll() like any other; nothing is silently dropped. */
		return -EFAULT;
	}

	return 0;
}

static long dma_accel_chr_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dma_accel_session *sess = filp->private_data;

	switch (cmd) {
	case DMA_ACCEL_IOC_BUFFER_ALLOC:
		return dma_accel_ioc_buffer_alloc(sess, arg);
	case DMA_ACCEL_IOC_SUBMIT:
		return dma_accel_ioc_submit(sess, arg);
	default:
		return -ENOTTY;
	}
}

static int dma_accel_chr_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct dma_accel_session *sess = filp->private_data;
	struct dma_accel_dev *dev = sess->dev;
	unsigned long buffer_id = vma->vm_pgoff; /* see mmap_offset encoding above */
	size_t req_size = vma->vm_end - vma->vm_start;
	int ret;

	if (buffer_id >= MAX_BUFFERS) {
		return -EINVAL;
	}

	mutex_lock(&dev->buf_mutex);
	if (!dev->buffers[buffer_id].in_use || req_size > dev->buffers[buffer_id].size) {
		mutex_unlock(&dev->buf_mutex);
		return -EINVAL;
	}
	/* M10.5: same ownership check as SUBMIT. buffer_id is a global
	 * slot index smuggled through the mmap() offset argument (see the
	 * comment below) — without this check, any fd could mmap any
	 * other fd's buffer straight from userspace, no SUBMIT required
	 * at all. */
	if (dev->buffers[buffer_id].owner != sess) {
		mutex_unlock(&dev->buf_mutex);
		return -EPERM;
	}

	/* vm_pgoff must be 0 for dma_mmap_coherent() — it's the buffer's
	 * OWN offset-within-mapping, unrelated to how we're (ab)using
	 * vm_pgoff above to smuggle the buffer_id through mmap()'s offset
	 * argument. */
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(&dev->pdev->dev, vma,
				 dev->buffers[buffer_id].kaddr,
				 dev->buffers[buffer_id].dma_addr,
				 dev->buffers[buffer_id].size);
	mutex_unlock(&dev->buf_mutex);

	return ret;
}

static ssize_t dma_accel_chr_read(struct file *filp, char __user *buf, size_t count,
				   loff_t *ppos)
{
	struct dma_accel_session *sess = filp->private_data;
	size_t n_avail, n_want, n_bytes;
	unsigned long flags;
	ssize_t copied = 0;

	/* M10.5: reads sess->comp_ring, not a device-wide ring — this fd
	 * can only ever see completions for commands it submitted itself.
	 * See dma_accel_irq_handler() for where that routing happens. */
	spin_lock_irqsave(&sess->comp_lock, flags);
	n_avail = sess->comp_tail - sess->comp_head;
	spin_unlock_irqrestore(&sess->comp_lock, flags);

	if (n_avail == 0) {
		return -EAGAIN; /* v0: read() is always non-blocking, use poll() to wait */
	}

	n_want = count / sizeof(struct dma_accel_completion_uapi);
	if (n_want == 0) {
		return -EINVAL; /* buffer too small for even one entry */
	}
	if (n_want > n_avail) {
		n_want = n_avail;
	}

	while ((size_t)copied < n_want) {
		struct dma_accel_completion_uapi out;
		u32 idx;

		spin_lock_irqsave(&sess->comp_lock, flags);
		if (sess->comp_head == sess->comp_tail) {
			spin_unlock_irqrestore(&sess->comp_lock, flags);
			break; /* raced with a concurrent reader; stop here */
		}
		idx = sess->comp_head % COMP_RING_SIZE;
		out.cmd_id = sess->comp_ring[idx].cmd_id;
		out.status = sess->comp_ring[idx].status;
		out.reserved = 0;
		sess->comp_head++;
		spin_unlock_irqrestore(&sess->comp_lock, flags);

		if (copy_to_user(buf + copied * sizeof(out), &out, sizeof(out))) {
			return copied > 0 ? (ssize_t)(copied * sizeof(out)) : -EFAULT;
		}
		copied++;
	}

	n_bytes = copied * sizeof(struct dma_accel_completion_uapi);
	*ppos += n_bytes;
	return (ssize_t)n_bytes;
}

static __poll_t dma_accel_chr_poll(struct file *filp, poll_table *wait)
{
	struct dma_accel_session *sess = filp->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	/* M10.5: waits on this session's own waitqueue — the ISR only wakes
	 * a session's comp_wq when it routes a completion to that specific
	 * session (see dma_accel_irq_handler()), so B's completions no
	 * longer wake A up to begin with. */
	poll_wait(filp, &sess->comp_wq, wait);

	spin_lock_irqsave(&sess->comp_lock, flags);
	if (sess->comp_head != sess->comp_tail) {
		mask |= EPOLLIN | EPOLLRDNORM;
	}
	spin_unlock_irqrestore(&sess->comp_lock, flags);

	return mask;
}

static const struct file_operations dma_accel_fops = {
	.owner          = THIS_MODULE,
	.open           = dma_accel_chr_open,
	.release        = dma_accel_chr_release,
	.unlocked_ioctl = dma_accel_chr_ioctl,
	.mmap           = dma_accel_chr_mmap,
	.read           = dma_accel_chr_read,
	.poll           = dma_accel_chr_poll,
};

static int dma_accel_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct dma_accel_dev *dev;
	u32 device_id;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	/* M9 state init — safe to do this early, before anything can
	 * possibly touch it (uapi_active stays false until the very end). */
	spin_lock_init(&dev->cmd_lock); /* M10.5: guards cmd_table, not comp_ring — completions now live per-session */
	mutex_init(&dev->buf_mutex);
	atomic64_set(&dev->next_cmd_id, 1);
	dev->uapi_active = false;
	/* M15: fairness-quota accounting, see the struct field comments. */
	atomic_set(&dev->active_sessions, 0);
	atomic_set(&dev->total_inflight, 0);
	init_waitqueue_head(&dev->quota_wq);

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pcim_enable_device failed: %d\n", ret);
		return ret;
	}

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "pcim_iomap_regions (BAR0) failed: %d\n", ret);
		return ret;
	}

	dev->bar0 = pcim_iomap_table(pdev)[0];
	if (!dev->bar0) {
		dev_err(&pdev->dev, "BAR0 iomap table entry is NULL\n");
		return -ENOMEM;
	}

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent failed: %d\n", ret);
		return ret;
	}

	device_id = ioread32(dev->bar0 + REG_DEVICE_ID);

	dev_info(&pdev->dev, "dma-accel probe: BAR0 mapped at %p, DEVICE_ID=0x%08x\n",
		 dev->bar0, device_id);

	if (device_id != DMA_ACCEL_DEVICE_ID_VAL) {
		dev_err(&pdev->dev,
			"DEVICE_ID mismatch: got 0x%08x, expected 0x%08x. "
			"BAR0 mapping or register offsets are wrong.\n",
			device_id, DMA_ACCEL_DEVICE_ID_VAL);
		return -ENODEV;
	}

	dev_info(&pdev->dev, "dma-accel: DEVICE_ID verified OK (M2 milestone passed)\n");

	/*
	 * M5: request one IRQ vector, MSI preferred (the device advertises
	 * MSI capability — see msi_init() on the QEMU side), falling back
	 * to legacy INTx if MSI can't be allocated for some reason.
	 */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	if (ret < 0) {
		dev_err(&pdev->dev, "pci_alloc_irq_vectors failed: %d\n", ret);
		return ret;
	}

	dev->irq = pci_irq_vector(pdev, 0);
	init_completion(&dev->irq_done);

	ret = request_irq(dev->irq, dma_accel_irq_handler, IRQF_SHARED,
			   DRV_NAME, dev);
	if (ret) {
		dev_err(&pdev->dev, "request_irq(%d) failed: %d\n", dev->irq, ret);
		pci_free_irq_vectors(pdev);
		return ret;
	}

	dev_info(&pdev->dev, "dma-accel: IRQ %d allocated (%s)\n", dev->irq,
		 pdev->msi_enabled ? "MSI" : "INTx");

	/* Unmask the two completion sources we care about in v0. */
	dma_accel_write_reg(dev, REG_IRQ_MASK, IRQ_DMA_DONE | IRQ_DMA_ERROR);

	ret = dma_accel_selftest(dev);
	if (ret) {
		dev_err(&pdev->dev, "M3 self-test failed with %d, probe aborted\n", ret);
		goto err_free_irq;
	}

	ret = dma_accel_error_path_tests(dev);
	if (ret) {
		dev_err(&pdev->dev, "M4 error-path tests failed with %d, probe aborted\n", ret);
		goto err_free_irq;
	}

	dev_info(&pdev->dev, "dma-accel: all M3/M4/M5 self-tests passed\n");

	/*
	 * M6: register the command queue. Must come after the M4 error-path
	 * tests' final dma_accel_do_reset() — RESET zeroes the device's
	 * sq/cq head/tail indices, so registering the queue before that
	 * reset would have it immediately clobbered.
	 */
	ret = dma_accel_queue_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "M6 queue_init failed with %d, probe aborted\n", ret);
		goto err_free_irq;
	}

	ret = dma_accel_queue_selftest(dev);
	if (ret) {
		dev_err(&pdev->dev, "M6 self-test failed with %d, probe aborted\n", ret);
		goto err_queue_fini;
	}

	dev_info(&pdev->dev, "dma-accel: all M3/M4/M5/M6 self-tests passed\n");

	ret = dma_accel_concurrency_test(dev);
	if (ret) {
		dev_err(&pdev->dev, "M7 concurrency test failed with %d, probe aborted\n", ret);
		goto err_queue_fini;
	}

	dev_info(&pdev->dev, "dma-accel: all M3/M4/M5/M6/M7 self-tests passed\n");

	ret = dma_accel_reset_test(dev);
	if (ret) {
		dev_err(&pdev->dev, "M8 reset test failed with %d, probe aborted\n", ret);
		goto err_queue_fini;
	}

	dev_info(&pdev->dev, "dma-accel: all M3/M4/M5/M6/M7/M8 self-tests passed\n");

	/*
	 * M9: register the character device only now, after every self-test
	 * has passed. This ordering is what keeps the M3-M8 self-test code
	 * completely untouched — no userspace client can possibly open the
	 * device (it doesn't exist yet) while probe() is still using the
	 * ISR in its original "just complete(&irq_done)" mode. Flip
	 * uapi_active first so the very next interrupt (which could in
	 * principle race with a userspace open() immediately after
	 * misc_register()) already drains into comp_ring correctly.
	 */
	dev->uapi_active = true;

	dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	dev->miscdev.name = "dma_accel0";
	dev->miscdev.fops = &dma_accel_fops;
	ret = misc_register(&dev->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "misc_register failed with %d, probe aborted\n", ret);
		dev->uapi_active = false;
		goto err_queue_fini;
	}

	dev_info(&pdev->dev, "dma-accel: /dev/%s ready\n", dev->miscdev.name);

	return 0;

err_queue_fini:
	dma_accel_queue_fini(dev);
err_free_irq:
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(pdev);
	return ret;
}

static void dma_accel_remove(struct pci_dev *pdev)
{
	struct dma_accel_dev *dev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "dma-accel remove\n");

	if (dev->uapi_active) {
		misc_deregister(&dev->miscdev);
	}
	dma_accel_queue_fini(dev);
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(pdev);
	/*
	 * pcim_* managed resources (BAR mapping, device enable) are released
	 * automatically by the devres framework after remove() returns.
	 */
}

static const struct pci_device_id dma_accel_ids[] = {
	{ PCI_DEVICE(DMA_ACCEL_PCI_VENDOR_ID, DMA_ACCEL_PCI_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, dma_accel_ids);

static struct pci_driver dma_accel_driver = {
	.name     = DRV_NAME,
	.id_table = dma_accel_ids,
	.probe    = dma_accel_probe,
	.remove   = dma_accel_remove,
};

module_pci_driver(dma_accel_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lingan");
MODULE_DESCRIPTION("dma-accel v0 PCIe driver (M3-M9: DMA, error paths, IRQ/MSI, queue, concurrency, reset, uAPI)");
