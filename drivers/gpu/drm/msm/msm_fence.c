/*
 * Copyright (C) 2013-2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/dma-fence.h>

#include "msm_drv.h"
#include "msm_fence.h"

struct msm_fence_context *
msm_fence_context_alloc(struct drm_device *dev, const char *name)
{
	struct msm_fence_context *fctx;

	fctx = kzalloc(sizeof(*fctx), GFP_KERNEL);
	if (!fctx)
		return ERR_PTR(-ENOMEM);

	fctx->dev = dev;
	strscpy(fctx->name, name, sizeof(fctx->name));
	fctx->context = dma_fence_context_alloc(1);
	fctx->timeline_value = 0; // Initialize timeline value
	init_waitqueue_head(&fctx->event);
	spin_lock_init(&fctx->spinlock);

	return fctx;
}

void msm_fence_context_free(struct msm_fence_context *fctx)
{
	kfree(fctx);
}

static inline bool timeline_completed(struct msm_fence_context *fctx, uint32_t timeline)
{
	return (int32_t)(fctx->timeline_value - timeline) >= 0;
}

/* legacy path for WAIT_FENCE ioctl: */
int msm_wait_timeline(struct msm_fence_context *fctx, uint32_t timeline,
		ktime_t *timeout, bool interruptible)
{
	int ret;

	if (timeline > fctx->timeline_value) {
		DRM_ERROR_RATELIMITED("%s: waiting on invalid timeline: %u (current: %u)\n",
				fctx->name, timeline, fctx->timeline_value);
		return -EINVAL;
	}

	if (!timeout) {
		/* no-wait: */
		ret = timeline_completed(fctx, timeline) ? 0 : -EBUSY;
	} else {
		unsigned long remaining_jiffies = timeout_to_jiffies(timeout);

		if (interruptible)
			ret = wait_event_interruptible_timeout(fctx->event,
				timeline_completed(fctx, timeline),
				remaining_jiffies);
		else
			ret = wait_event_timeout(fctx->event,
				timeline_completed(fctx, timeline),
				remaining_jiffies);

		if (ret == 0) {
			DBG("timeout waiting for timeline: %u (current: %u)",
					timeline, fctx->timeline_value);
			ret = -ETIMEDOUT;
		} else if (ret != -ERESTARTSYS) {
			ret = 0;
		}
	}

	return ret;
}

/* called from workqueue */
void msm_update_timeline(struct msm_fence_context *fctx, uint32_t timeline)
{
	spin_lock(&fctx->spinlock);
	fctx->timeline_value = max(timeline, fctx->timeline_value);
	spin_unlock(&fctx->spinlock);

	wake_up_all(&fctx->event);
}

struct msm_fence {
	struct dma_fence base;
	struct msm_fence_context *fctx;
};

static inline struct msm_fence *to_msm_fence(struct dma_fence *fence)
{
	return container_of(fence, struct msm_fence, base);
}

static const char *msm_fence_get_driver_name(struct dma_fence *fence)
{
	return "msm";
}

static const char *msm_fence_get_timeline_name(struct dma_fence *fence)
{
	struct msm_fence *f = to_msm_fence(fence);
	return f->fctx->name;
}

static bool msm_fence_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static bool msm_fence_signaled(struct dma_fence *fence)
{
	struct msm_fence *f = to_msm_fence(fence);
	return timeline_completed(f->fctx, f->base.seqno);
}

static const struct dma_fence_ops msm_fence_ops = {
	.get_driver_name = msm_fence_get_driver_name,
	.get_timeline_name = msm_fence_get_timeline_name,
	.enable_signaling = msm_fence_enable_signaling,
	.signaled = msm_fence_signaled,
	.wait = dma_fence_default_wait,
	.release = dma_fence_free,
};

struct dma_fence *
msm_fence_alloc(struct msm_fence_context *fctx)
{
	struct msm_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return ERR_PTR(-ENOMEM);

	f->fctx = fctx;

	dma_fence_init(&f->base, &msm_fence_ops, &fctx->spinlock,
		       fctx->context, ++fctx->last_fence);

	return &f->base;
}

