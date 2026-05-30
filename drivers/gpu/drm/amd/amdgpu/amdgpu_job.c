/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include <drm/drm_drv.h>

#include "amdgpu.h"
#include "amdgpu_trace.h"
#include "amdgpu_reset.h"
#include "amdgpu_dev_coredump.h"
#include "amdgpu_xgmi.h"

static void amdgpu_job_do_core_dump(struct amdgpu_device *adev,
				    struct amdgpu_job *job)
{
	int i;

	dev_info(adev->dev, "Dumping IP State\n");
	for (i = 0; i < adev->num_ip_blocks; i++)
		if (adev->ip_blocks[i].version->funcs->dump_ip_state)
			adev->ip_blocks[i].version->funcs
				->dump_ip_state((void *)&adev->ip_blocks[i]);
	dev_info(adev->dev, "Dumping IP State Completed\n");

	amdgpu_coredump(adev, true, false, job);
}

static void amdgpu_job_core_dump(struct amdgpu_device *adev,
				 struct amdgpu_job *job)
{
	struct list_head device_list, *device_list_handle =  NULL;
	struct amdgpu_device *tmp_adev = NULL;
	struct amdgpu_hive_info *hive = NULL;

	if (!amdgpu_sriov_vf(adev))
		hive = amdgpu_get_xgmi_hive(adev);
	if (hive)
		mutex_lock(&hive->hive_lock);
	/*
	 * Reuse the logic in amdgpu_device_gpu_recover() to build list of
	 * devices for code dump
	 */
	INIT_LIST_HEAD(&device_list);
	if (!amdgpu_sriov_vf(adev) && (adev->gmc.xgmi.num_physical_nodes > 1) && hive) {
		list_for_each_entry(tmp_adev, &hive->device_list, gmc.xgmi.head)
			list_add_tail(&tmp_adev->reset_list, &device_list);
		if (!list_is_first(&adev->reset_list, &device_list))
			list_rotate_to_front(&adev->reset_list, &device_list);
		device_list_handle = &device_list;
	} else {
		list_add_tail(&adev->reset_list, &device_list);
		device_list_handle = &device_list;
	}

	/* Do the coredump for each device */
	list_for_each_entry(tmp_adev, device_list_handle, reset_list)
		amdgpu_job_do_core_dump(tmp_adev, job);

	if (hive) {
		mutex_unlock(&hive->hive_lock);
		amdgpu_put_xgmi_hive(hive);
	}
}

static enum drm_gpu_sched_stat amdgpu_job_timedout(struct drm_sched_job *s_job)
{
	struct amdgpu_ring *ring = to_amdgpu_ring(s_job->sched);
	struct amdgpu_job *job = to_amdgpu_job(s_job);
	struct amdgpu_task_info *ti;
	struct amdgpu_device *adev = ring->adev;
	int idx;
	int r;

	if (!drm_dev_enter(adev_to_drm(adev), &idx)) {
		dev_info(adev->dev, "%s - device unplugged skipping recovery on scheduler:%s",
			 __func__, s_job->sched->name);

		/* Effectively the job is aborted as the device is gone */
		return DRM_GPU_SCHED_STAT_ENODEV;
	}

	/*
	 * Diagnostic: identify exactly which ring/job/pasid tripped the
	 * timeout, logged before any recovery or coredump runs so it is
	 * always captured. signaled_seq is the last fence the HW completed;
	 * emitted_seq is the last fence submitted - a gap means the ring
	 * stalled mid-stream.
	 */
	dev_err(adev->dev,
		"DEBUG_TIMEOUT ring=%s pasid=%u vmid=%u signaled_seq=%u emitted_seq=%u job=%p\n",
		ring->name, job->pasid, job->vmid,
		atomic_read(&ring->fence_drv.last_seq),
		ring->fence_drv.sync_seq, job);

	/*
	 * Diagnostic: dump the IB(s) of the timed-out job so we can see what
	 * command stream the ring was running when it stopped making forward
	 * progress. The job is still live in its own timeout handler, so the
	 * IB suballocation has not been freed and ib->ptr is safe to read.
	 * Diagnostic only - no behaviour change.
	 */
	{
		unsigned int ib_i, dw_i, dump_dw;
		struct amdgpu_ib *ib;

		dev_err(adev->dev, "DEBUG_IB ring=%s pasid=%u vmid=%u num_ibs=%u\n",
			ring->name, job->pasid, job->vmid, job->num_ibs);

		for (ib_i = 0; ib_i < job->num_ibs; ib_i++) {
			ib = &job->ibs[ib_i];

			dev_err(adev->dev,
				"DEBUG_IB[%u] gpu_addr=0x%llx length_dw=%u flags=0x%x ptr=%p\n",
				ib_i, ib->gpu_addr, ib->length_dw, ib->flags,
				ib->ptr);

			if (!ib->ptr || !ib->length_dw)
				continue;

			/* dump up to the first 32 dwords, 8 per line */
			dump_dw = min_t(unsigned int, ib->length_dw, 32u);
			for (dw_i = 0; dw_i < dump_dw; dw_i += 8) {
				char buf[8 * 11 + 1];
				unsigned int k;
				int pos = 0;

				for (k = 0; k < 8 && (dw_i + k) < dump_dw; k++)
					pos += scnprintf(buf + pos,
							 sizeof(buf) - pos,
							 "0x%08x ",
							 ib->ptr[dw_i + k]);
				dev_err(adev->dev, "DEBUG_IB[%u] DW%u: %s\n",
					ib_i, dw_i, buf);
			}
		}
	}

	/*
	 * DEBUG_RING_IB: the DRM scheduler blames the oldest unsignalled job,
	 * which is not necessarily the frame the CP is physically stuck in.
	 * Read the hardware rptr and decode the PM4 packets the ring is
	 * actually executing there to find the INDIRECT_BUFFER the GPU is
	 * running, then compare its address against the timed-out job's IBs.
	 *
	 * ring->ring is the kernel CPU mapping of the ring BO, so reading it
	 * is safe and sleep-free here. We index it exactly like the kernel's
	 * own ring reader: ring->ring[get_rptr() & buf_mask] (buf_mask and
	 * rptr are in dwords). The user IBs the ring points to are not
	 * kernel-mapped (that is why DEBUG_IB shows ptr=NULL for gfx), so we
	 * decode the ring frame, not the IB body. PM4 decode only makes sense
	 * for gfx/compute rings. Diagnostic only - no behaviour change.
	 */
	if (ring->ring && ring->buf_mask &&
	    (ring->funcs->type == AMDGPU_RING_TYPE_GFX ||
	     ring->funcs->type == AMDGPU_RING_TYPE_COMPUTE)) {
		u32 mask = ring->buf_mask;
		u32 rptr = amdgpu_ring_get_rptr(ring) & mask;
		u32 wptr = amdgpu_ring_get_wptr(ring) & mask;
		unsigned int scan, found = 0, m;

		dev_err(adev->dev,
			"DEBUG_RING_IB ring=%s rptr=0x%x (byte 0x%x) wptr=0x%x buf_mask=0x%x num_ibs=%u\n",
			ring->name, rptr, rptr << 2, wptr, mask, job->num_ibs);

		/* reprint the job IBs here so the comparison is self-contained */
		for (m = 0; m < job->num_ibs; m++)
			dev_err(adev->dev,
				"DEBUG_RING_IB job_ib[%u] addr=0x%llx len_dw=%u\n",
				m, job->ibs[m].gpu_addr, job->ibs[m].length_dw);

		/* raw 16-dword window at rptr for context */
		for (scan = 0; scan < 16; scan++) {
			u32 idx = (rptr + scan) & mask;

			dev_err(adev->dev,
				"DEBUG_RING_IB win +%2u idx=0x%x val=0x%08x\n",
				scan, idx, ring->ring[idx]);
		}

		/*
		 * Walk PM4 type-3 packets forward from rptr looking for
		 * INDIRECT_BUFFER (0x3f) / INDIRECT_BUFFER_CONST (0x33).
		 */
		scan = 0;
		while (scan < 512) {
			u32 idx = (rptr + scan) & mask;
			u32 header = ring->ring[idx];
			u32 count, op;

			if ((header >> 30) != 3) {	/* not a type-3 header */
				scan++;
				continue;
			}
			count = ((header >> 16) & 0x3fff) + 1;
			op = (header >> 8) & 0xff;

			if (op == 0x3f || op == 0x33) {	/* INDIRECT_BUFFER[_CONST] */
				u32 lo = ring->ring[(idx + 1) & mask];
				u32 hi = ring->ring[(idx + 2) & mask];
				u32 ctl = ring->ring[(idx + 3) & mask];
				u64 ib_addr = ((u64)hi << 32) | (lo & 0xfffffffc);
				int match = -1;

				for (m = 0; m < job->num_ibs; m++) {
					if (job->ibs[m].gpu_addr == ib_addr) {
						match = (int)m;
						break;
					}
				}

				dev_err(adev->dev,
					"DEBUG_RING_IB[%u] op=0x%02x ring_idx=0x%x ib_addr=0x%llx size_dw=%u ctl=0x%08x job_ib_match=%d\n",
					found, op, idx, ib_addr, ctl & 0xfffff,
					ctl, match);
				found++;
			}
			scan += count + 1;
		}
		if (!found)
			dev_err(adev->dev,
				"DEBUG_RING_IB no INDIRECT_BUFFER found in scan window\n");
	}

	/*
	 * DEBUG_FENCE: the gfx pipeline-sync WAIT_REG_MEM the CP parks on
	 * waits for this ring's fence writeback (fence_drv.gpu_addr, e.g.
	 * 0x401080) to equal sync_seq. cpu_addr is a coherent kernel mirror
	 * of that exact wb slot, so read the live value and compare it to the
	 * awaited seq to classify the stall:
	 *   live <  sync_seq  -> fence never written (work did not retire)
	 *   live == sync_seq  -> value present but CP not advancing (coherency)
	 *   live >  sync_seq  -> fence overshot the awaited == value (skip/order)
	 * Diagnostic only - no behaviour change.
	 */
	{
		u32 fence_live = ring->fence_drv.cpu_addr ?
			le32_to_cpu(*ring->fence_drv.cpu_addr) : 0xdeadbeef;

		dev_err(adev->dev,
			"DEBUG_FENCE ring=%s fence_addr=0x%llx live=0x%x sync_seq=0x%x last_seq=0x%x\n",
			ring->name, ring->fence_drv.gpu_addr, fence_live,
			ring->fence_drv.sync_seq,
			(u32)atomic_read(&ring->fence_drv.last_seq));
	}

	/*
	 * DEBUG_GRBM: on the PS4 (CIK/gfx7) read the GPU busy-status registers
	 * to tell a shader/pipeline hang (still churning) apart from the CP
	 * merely parked on a fence wait (pipeline idle). Offsets are the CIK
	 * gfx_7_2 ones (GRBM_STATUS/STATUS2/CP_STAT); only read them on the
	 * matching ASIC so this stays safe on other GPUs. GUI_ACTIVE is bit 31
	 * of GRBM_STATUS. Diagnostic only.
	 */
	if (adev->asic_type == CHIP_LIVERPOOL ||
	    adev->asic_type == CHIP_GLADIUS) {
		u32 grbm   = RREG32(0x2004); /* mmGRBM_STATUS  */
		u32 grbm2  = RREG32(0x2002); /* mmGRBM_STATUS2 */
		u32 cpstat = RREG32(0x21a0); /* mmCP_STAT      */

		dev_err(adev->dev,
			"DEBUG_GRBM ring=%s GRBM_STATUS=0x%08x GRBM_STATUS2=0x%08x CP_STAT=0x%08x gui_active=%u\n",
			ring->name, grbm, grbm2, cpstat, (grbm >> 31) & 1);
	}

	/*
	 * Do the coredump immediately after a job timeout to get a very
	 * close dump/snapshot/representation of GPU's current error status
	 * Skip it for SRIOV, since VF FLR will be triggered by host driver
	 * before job timeout
	 */
	if (!amdgpu_sriov_vf(adev))
		amdgpu_job_core_dump(adev, job);

	if (amdgpu_gpu_recovery &&
	    amdgpu_ring_soft_recovery(ring, job->vmid, s_job->s_fence->parent)) {
		dev_err(adev->dev, "ring %s timeout, but soft recovered\n",
			s_job->sched->name);
		goto exit;
	}

	dev_err(adev->dev, "ring %s timeout, signaled seq=%u, emitted seq=%u\n",
		job->base.sched->name, atomic_read(&ring->fence_drv.last_seq),
		ring->fence_drv.sync_seq);

	ti = amdgpu_vm_get_task_info_pasid(ring->adev, job->pasid);
	if (ti) {
		dev_err(adev->dev,
			"Process information: process %s pid %d thread %s pid %d\n",
			ti->process_name, ti->tgid, ti->task_name, ti->pid);
		amdgpu_vm_put_task_info(ti);
	}

	/* attempt a per ring reset */
	if (unlikely(adev->debug_disable_gpu_ring_reset)) {
		dev_err(adev->dev, "Ring reset disabled by debug mask\n");
	} else if (amdgpu_gpu_recovery && ring->funcs->reset) {
		bool is_guilty;

		dev_err(adev->dev, "Starting %s ring reset\n", s_job->sched->name);
		/* stop the scheduler, but don't mess with the
		 * bad job yet because if ring reset fails
		 * we'll fall back to full GPU reset.
		 */
		drm_sched_wqueue_stop(&ring->sched);

		/* for engine resets, we need to reset the engine,
		 * but individual queues may be unaffected.
		 * check here to make sure the accounting is correct.
		 */
		if (ring->funcs->is_guilty)
			is_guilty = ring->funcs->is_guilty(ring);
		else
			is_guilty = true;

		if (is_guilty)
			dma_fence_set_error(&s_job->s_fence->finished, -ETIME);

		r = amdgpu_ring_reset(ring, job->vmid);
		if (!r) {
			if (amdgpu_ring_sched_ready(ring))
				drm_sched_stop(&ring->sched, s_job);
			if (is_guilty) {
				atomic_inc(&ring->adev->gpu_reset_counter);
				amdgpu_fence_driver_force_completion(ring);
			}
			if (amdgpu_ring_sched_ready(ring))
				drm_sched_start(&ring->sched, 0);
			dev_err(adev->dev, "Ring %s reset succeeded\n", ring->sched.name);
			drm_dev_wedged_event(adev_to_drm(adev), DRM_WEDGE_RECOVERY_NONE);
			goto exit;
		}
		dev_err(adev->dev, "Ring %s reset failure\n", ring->sched.name);
	}
	dma_fence_set_error(&s_job->s_fence->finished, -ETIME);

	if (amdgpu_device_should_recover_gpu(ring->adev)) {
		struct amdgpu_reset_context reset_context;
		memset(&reset_context, 0, sizeof(reset_context));

		reset_context.method = AMD_RESET_METHOD_NONE;
		reset_context.reset_req_dev = adev;
		reset_context.src = AMDGPU_RESET_SRC_JOB;
		clear_bit(AMDGPU_NEED_FULL_RESET, &reset_context.flags);

		/*
		 * To avoid an unnecessary extra coredump, as we have already
		 * got the very close representation of GPU's error status
		 */
		set_bit(AMDGPU_SKIP_COREDUMP, &reset_context.flags);

		r = amdgpu_device_gpu_recover(ring->adev, job, &reset_context);
		if (r)
			dev_err(adev->dev, "GPU Recovery Failed: %d\n", r);
	} else {
		drm_sched_suspend_timeout(&ring->sched);
		if (amdgpu_sriov_vf(adev))
			adev->virt.tdr_debug = true;
	}

exit:
	drm_dev_exit(idx);
	return DRM_GPU_SCHED_STAT_NOMINAL;
}

int amdgpu_job_alloc(struct amdgpu_device *adev, struct amdgpu_vm *vm,
		     struct drm_sched_entity *entity, void *owner,
		     unsigned int num_ibs, struct amdgpu_job **job)
{
	if (num_ibs == 0)
		return -EINVAL;

	*job = kzalloc(struct_size(*job, ibs, num_ibs), GFP_KERNEL);
	if (!*job)
		return -ENOMEM;

	(*job)->vm = vm;

	amdgpu_sync_create(&(*job)->explicit_sync);
	(*job)->generation = amdgpu_vm_generation(adev, vm);
	(*job)->vm_pd_addr = AMDGPU_BO_INVALID_OFFSET;

	if (!entity)
		return 0;

	return drm_sched_job_init(&(*job)->base, entity, 1, owner);
}

int amdgpu_job_alloc_with_ib(struct amdgpu_device *adev,
			     struct drm_sched_entity *entity, void *owner,
			     size_t size, enum amdgpu_ib_pool_type pool_type,
			     struct amdgpu_job **job)
{
	int r;

	r = amdgpu_job_alloc(adev, NULL, entity, owner, 1, job);
	if (r)
		return r;

	(*job)->num_ibs = 1;
	r = amdgpu_ib_get(adev, NULL, size, pool_type, &(*job)->ibs[0]);
	if (r) {
		if (entity)
			drm_sched_job_cleanup(&(*job)->base);
		kfree(*job);
	}

	return r;
}

void amdgpu_job_set_resources(struct amdgpu_job *job, struct amdgpu_bo *gds,
			      struct amdgpu_bo *gws, struct amdgpu_bo *oa)
{
	if (gds) {
		job->gds_base = amdgpu_bo_gpu_offset(gds) >> PAGE_SHIFT;
		job->gds_size = amdgpu_bo_size(gds) >> PAGE_SHIFT;
	}
	if (gws) {
		job->gws_base = amdgpu_bo_gpu_offset(gws) >> PAGE_SHIFT;
		job->gws_size = amdgpu_bo_size(gws) >> PAGE_SHIFT;
	}
	if (oa) {
		job->oa_base = amdgpu_bo_gpu_offset(oa) >> PAGE_SHIFT;
		job->oa_size = amdgpu_bo_size(oa) >> PAGE_SHIFT;
	}
}

void amdgpu_job_free_resources(struct amdgpu_job *job)
{
	struct dma_fence *f;
	unsigned i;

	/* Check if any fences where initialized */
	if (job->base.s_fence && job->base.s_fence->finished.ops)
		f = &job->base.s_fence->finished;
	else if (job->hw_fence.ops)
		f = &job->hw_fence;
	else
		f = NULL;

	for (i = 0; i < job->num_ibs; ++i)
		amdgpu_ib_free(&job->ibs[i], f);
}

static void amdgpu_job_free_cb(struct drm_sched_job *s_job)
{
	struct amdgpu_job *job = to_amdgpu_job(s_job);

	drm_sched_job_cleanup(s_job);

	amdgpu_sync_free(&job->explicit_sync);

	/* only put the hw fence if has embedded fence */
	if (!job->hw_fence.ops)
		kfree(job);
	else
		dma_fence_put(&job->hw_fence);
}

void amdgpu_job_set_gang_leader(struct amdgpu_job *job,
				struct amdgpu_job *leader)
{
	struct dma_fence *fence = &leader->base.s_fence->scheduled;

	WARN_ON(job->gang_submit);

	/*
	 * Don't add a reference when we are the gang leader to avoid circle
	 * dependency.
	 */
	if (job != leader)
		dma_fence_get(fence);
	job->gang_submit = fence;
}

void amdgpu_job_free(struct amdgpu_job *job)
{
	if (job->base.entity)
		drm_sched_job_cleanup(&job->base);

	amdgpu_job_free_resources(job);
	amdgpu_sync_free(&job->explicit_sync);
	if (job->gang_submit != &job->base.s_fence->scheduled)
		dma_fence_put(job->gang_submit);

	if (!job->hw_fence.ops)
		kfree(job);
	else
		dma_fence_put(&job->hw_fence);
}

struct dma_fence *amdgpu_job_submit(struct amdgpu_job *job)
{
	struct dma_fence *f;

	drm_sched_job_arm(&job->base);
	f = dma_fence_get(&job->base.s_fence->finished);
	amdgpu_job_free_resources(job);
	drm_sched_entity_push_job(&job->base);

	return f;
}

int amdgpu_job_submit_direct(struct amdgpu_job *job, struct amdgpu_ring *ring,
			     struct dma_fence **fence)
{
	int r;

	job->base.sched = &ring->sched;
	r = amdgpu_ib_schedule(ring, job->num_ibs, job->ibs, job, fence);

	if (r)
		return r;

	amdgpu_job_free(job);
	return 0;
}

static struct dma_fence *
amdgpu_job_prepare_job(struct drm_sched_job *sched_job,
		      struct drm_sched_entity *s_entity)
{
	struct amdgpu_ring *ring = to_amdgpu_ring(s_entity->rq->sched);
	struct amdgpu_job *job = to_amdgpu_job(sched_job);
	struct dma_fence *fence;
	int r;

	r = drm_sched_entity_error(s_entity);
	if (r)
		goto error;

	if (job->gang_submit) {
		fence = amdgpu_device_switch_gang(ring->adev, job->gang_submit);
		if (fence)
			return fence;
	}

	fence = amdgpu_device_enforce_isolation(ring->adev, ring, job);
	if (fence)
		return fence;

	if (job->vm && !job->vmid) {
		r = amdgpu_vmid_grab(job->vm, ring, job, &fence);
		if (r) {
			dev_err(ring->adev->dev, "Error getting VM ID (%d)\n", r);
			goto error;
		}
		/*
		 * The VM structure might be released after the VMID is
		 * assigned, we had multiple problems with people trying to use
		 * the VM pointer so better set it to NULL.
		 */
		if (!fence)
			job->vm = NULL;
		return fence;
	}

	return NULL;

error:
	dma_fence_set_error(&job->base.s_fence->finished, r);
	return NULL;
}

static struct dma_fence *amdgpu_job_run(struct drm_sched_job *sched_job)
{
	struct amdgpu_ring *ring = to_amdgpu_ring(sched_job->sched);
	struct amdgpu_device *adev = ring->adev;
	struct dma_fence *fence = NULL, *finished;
	struct amdgpu_job *job;
	int r = 0;

	job = to_amdgpu_job(sched_job);
	finished = &job->base.s_fence->finished;

	trace_amdgpu_sched_run_job(job);

	/* Skip job if VRAM is lost and never resubmit gangs */
	if (job->generation != amdgpu_vm_generation(adev, job->vm) ||
	    (job->job_run_counter && job->gang_submit))
		dma_fence_set_error(finished, -ECANCELED);

	if (finished->error < 0) {
		dev_dbg(adev->dev, "Skip scheduling IBs in ring(%s)",
			ring->name);
	} else {
		r = amdgpu_ib_schedule(ring, job->num_ibs, job->ibs, job,
				       &fence);
		if (r)
			dev_err(adev->dev,
				"Error scheduling IBs (%d) in ring(%s)", r,
				ring->name);
	}

	job->job_run_counter++;
	amdgpu_job_free_resources(job);

	fence = r ? ERR_PTR(r) : fence;
	return fence;
}

/*
 * This is a duplicate function from DRM scheduler sched_internal.h.
 * Plan is to remove it when amdgpu_job_stop_all_jobs_on_sched is removed, due
 * latter being incorrect and racy.
 *
 * See https://lore.kernel.org/amd-gfx/44edde63-7181-44fb-a4f7-94e50514f539@amd.com/
 */
static struct drm_sched_job *
drm_sched_entity_queue_pop(struct drm_sched_entity *entity)
{
	struct spsc_node *node;

	node = spsc_queue_pop(&entity->job_queue);
	if (!node)
		return NULL;

	return container_of(node, struct drm_sched_job, queue_node);
}

void amdgpu_job_stop_all_jobs_on_sched(struct drm_gpu_scheduler *sched)
{
	struct drm_sched_job *s_job;
	struct drm_sched_entity *s_entity = NULL;
	int i;

	/* Signal all jobs not yet scheduled */
	for (i = DRM_SCHED_PRIORITY_KERNEL; i < sched->num_rqs; i++) {
		struct drm_sched_rq *rq = sched->sched_rq[i];
		spin_lock(&rq->lock);
		list_for_each_entry(s_entity, &rq->entities, list) {
			while ((s_job = drm_sched_entity_queue_pop(s_entity))) {
				struct drm_sched_fence *s_fence = s_job->s_fence;

				dma_fence_signal(&s_fence->scheduled);
				dma_fence_set_error(&s_fence->finished, -EHWPOISON);
				dma_fence_signal(&s_fence->finished);
			}
		}
		spin_unlock(&rq->lock);
	}

	/* Signal all jobs already scheduled to HW */
	list_for_each_entry(s_job, &sched->pending_list, list) {
		struct drm_sched_fence *s_fence = s_job->s_fence;

		dma_fence_set_error(&s_fence->finished, -EHWPOISON);
		dma_fence_signal(&s_fence->finished);
	}
}

const struct drm_sched_backend_ops amdgpu_sched_ops = {
	.prepare_job = amdgpu_job_prepare_job,
	.run_job = amdgpu_job_run,
	.timedout_job = amdgpu_job_timedout,
	.free_job = amdgpu_job_free_cb
};
