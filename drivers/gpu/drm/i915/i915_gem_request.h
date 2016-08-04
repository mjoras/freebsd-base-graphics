/*
 * Copyright © 2008-2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef I915_GEM_REQUEST_H
#define I915_GEM_REQUEST_H

#include <linux/fence.h>

#include "i915_gem.h"

/**
 * Request queue structure.
 *
 * The request queue allows us to note sequence numbers that have been emitted
 * and may be associated with active buffers to be retired.
 *
 * By keeping this list, we can avoid having to do questionable sequence
 * number comparisons on buffer last_read|write_seqno. It also allows an
 * emission time to be associated with the request for tracking how far ahead
 * of the GPU the submission is.
 *
 * The requests are reference counted.
 */
struct drm_i915_gem_request {
	struct fence fence;
	spinlock_t lock;

	/** On Which ring this request was generated */
	struct drm_i915_private *i915;

	/**
	 * Context and ring buffer related to this request
	 * Contexts are refcounted, so when this request is associated with a
	 * context, we must increment the context's refcount, to guarantee that
	 * it persists while any request is linked to it. Requests themselves
	 * are also refcounted, so the request will only be freed when the last
	 * reference to it is dismissed, and the code in
	 * i915_gem_request_free() will then decrement the refcount on the
	 * context.
	 */
	struct i915_gem_context *ctx;
	struct intel_engine_cs *engine;
	struct intel_ring *ring;
	struct intel_signal_node signaling;

	/** GEM sequence number associated with the previous request,
	 * when the HWS breadcrumb is equal to this the GPU is processing
	 * this request.
	 */
	u32 previous_seqno;

	/** Position in the ringbuffer of the start of the request */
	u32 head;

	/**
	 * Position in the ringbuffer of the start of the postfix.
	 * This is required to calculate the maximum available ringbuffer
	 * space without overwriting the postfix.
	 */
	u32 postfix;

	/** Position in the ringbuffer of the end of the whole request */
	u32 tail;

	/** Preallocate space in the ringbuffer for the emitting the request */
	u32 reserved_space;

	/**
	 * Context related to the previous request.
	 * As the contexts are accessed by the hardware until the switch is
	 * completed to a new context, the hardware may still be writing
	 * to the context object after the breadcrumb is visible. We must
	 * not unpin/unbind/prune that object whilst still active and so
	 * we keep the previous context pinned until the following (this)
	 * request is retired.
	 */
	struct i915_gem_context *previous_context;

	/** Batch buffer related to this request if any (used for
	 * error state dump only).
	 */
	struct drm_i915_gem_object *batch_obj;

	/** Time at which this request was emitted, in jiffies. */
	unsigned long emitted_jiffies;

	/** global list entry for this request */
	struct list_head list;

	struct drm_i915_file_private *file_priv;
	/** file_priv list entry for this request */
	struct list_head client_list;

	/** process identifier submitting this request */
#ifdef __linux__
	struct pid *pid;
#else
	pid_t pid;
#endif

	/**
	 * The ELSP only accepts two elements at a time, so we queue
	 * context/tail pairs on a given queue (ring->execlist_queue) until the
	 * hardware is available. The queue serves a double purpose: we also use
	 * it to keep track of the up to 2 contexts currently in the hardware
	 * (usually one in execution and the other queued up by the GPU): We
	 * only remove elements from the head of the queue when the hardware
	 * informs us that an element has been completed.
	 *
	 * All accesses to the queue are mediated by a spinlock
	 * (ring->execlist_lock).
	 */

	/** Execlist link in the submission queue.*/
	struct list_head execlist_link;

	/** Execlists no. of times this request has been sent to the ELSP */
	int elsp_submitted;

	/** Execlists context hardware id. */
	unsigned int ctx_hw_id;
};

extern const struct fence_ops i915_fence_ops;

static inline bool fence_is_i915(struct fence *fence)
{
	return fence->ops == &i915_fence_ops;
}

struct drm_i915_gem_request * __must_check
i915_gem_request_alloc(struct intel_engine_cs *engine,
		       struct i915_gem_context *ctx);
int i915_gem_request_add_to_client(struct drm_i915_gem_request *req,
				   struct drm_file *file);
void i915_gem_request_retire_upto(struct drm_i915_gem_request *req);

static inline u32
i915_gem_request_get_seqno(struct drm_i915_gem_request *req)
{
	return req ? req->fence.seqno : 0;
}

static inline struct intel_engine_cs *
i915_gem_request_get_engine(struct drm_i915_gem_request *req)
{
	return req ? req->engine : NULL;
}

static inline struct drm_i915_gem_request *
to_request(struct fence *fence)
{
	/* We assume that NULL fence/request are interoperable */
	BUILD_BUG_ON(offsetof(struct drm_i915_gem_request, fence) != 0);
	GEM_BUG_ON(fence && !fence_is_i915(fence));
	return container_of(fence, struct drm_i915_gem_request, fence);
}

static inline struct drm_i915_gem_request *
i915_gem_request_get(struct drm_i915_gem_request *req)
{
	return to_request(fence_get(&req->fence));
}

static inline void
i915_gem_request_put(struct drm_i915_gem_request *req)
{
	fence_put(&req->fence);
}

static inline void i915_gem_request_assign(struct drm_i915_gem_request **pdst,
					   struct drm_i915_gem_request *src)
{
	if (src)
		i915_gem_request_get(src);

	if (*pdst)
		i915_gem_request_put(*pdst);

	*pdst = src;
}

void __i915_add_request(struct drm_i915_gem_request *req,
			struct drm_i915_gem_object *batch_obj,
			bool flush_caches);
#define i915_add_request(req) \
	__i915_add_request(req, NULL, true)
#define i915_add_request_no_flush(req) \
	__i915_add_request(req, NULL, false)

struct intel_rps_client;
#define NO_WAITBOOST ERR_PTR(-1)
#define IS_RPS_CLIENT(p) (!IS_ERR(p))
#define IS_RPS_USER(p) (!IS_ERR_OR_NULL(p))

int __i915_wait_request(struct drm_i915_gem_request *req,
			bool interruptible,
			s64 *timeout,
			struct intel_rps_client *rps);
int __must_check i915_wait_request(struct drm_i915_gem_request *req);

static inline u32 intel_engine_get_seqno(struct intel_engine_cs *engine);

/**
 * Returns true if seq1 is later than seq2.
 */
static inline bool i915_seqno_passed(u32 seq1, u32 seq2)
{
	return (s32)(seq1 - seq2) >= 0;
}

static inline bool
i915_gem_request_started(const struct drm_i915_gem_request *req)
{
	return i915_seqno_passed(intel_engine_get_seqno(req->engine),
				 req->previous_seqno);
}

static inline bool
i915_gem_request_completed(const struct drm_i915_gem_request *req)
{
	return i915_seqno_passed(intel_engine_get_seqno(req->engine),
				 req->fence.seqno);
}

bool __i915_spin_request(const struct drm_i915_gem_request *request,
			 int state, unsigned long timeout_us);
static inline bool i915_spin_request(const struct drm_i915_gem_request *request,
				     int state, unsigned long timeout_us)
{
	return (i915_gem_request_started(request) &&
		__i915_spin_request(request, state, timeout_us));
}

/* We treat requests as fences. This is not be to confused with our
 * "fence registers" but pipeline synchronisation objects ala GL_ARB_sync.
 * We use the fences to synchronize access from the CPU with activity on the
 * GPU, for example, we should not rewrite an object's PTE whilst the GPU
 * is reading them. We also track fences at a higher level to provide
 * implicit synchronisation around GEM objects, e.g. set-domain will wait
 * for outstanding GPU rendering before marking the object ready for CPU
 * access, or a pageflip will wait until the GPU is complete before showing
 * the frame on the scanout.
 *
 * In order to use a fence, the object must track the fence it needs to
 * serialise with. For example, GEM objects want to track both read and
 * write access so that we can perform concurrent read operations between
 * the CPU and GPU engines, as well as waiting for all rendering to
 * complete, or waiting for the last GPU user of a "fence register". The
 * object then embeds a #i915_gem_active to track the most recent (in
 * retirement order) request relevant for the desired mode of access.
 * The #i915_gem_active is updated with i915_gem_active_set() to track the
 * most recent fence request, typically this is done as part of
 * i915_vma_move_to_active().
 *
 * When the #i915_gem_active completes (is retired), it will
 * signal its completion to the owner through a callback as well as mark
 * itself as idle (i915_gem_active.request == NULL). The owner
 * can then perform any action, such as delayed freeing of an active
 * resource including itself.
 */
struct i915_gem_active {
	struct drm_i915_gem_request *request;
};

/**
 * i915_gem_active_set - updates the tracker to watch the current request
 * @active - the active tracker
 * @request - the request to watch
 *
 * i915_gem_active_set() watches the given @request for completion. Whilst
 * that @request is busy, the @active reports busy. When that @request is
 * retired, the @active tracker is updated to report idle.
 */
static inline void
i915_gem_active_set(struct i915_gem_active *active,
		    struct drm_i915_gem_request *request)
{
	i915_gem_request_assign(&active->request, request);
}

/**
 * i915_gem_active_peek - report the request being monitored
 * @active - the active tracker
 *
 * i915_gem_active_peek() returns the current request being tracked, or NULL.
 * It does not obtain a reference on the request for the caller, so the
 * caller must hold struct_mutex.
 */
static inline struct drm_i915_gem_request *
i915_gem_active_peek(const struct i915_gem_active *active)
{
	return active->request;
}

/**
 * i915_gem_active_get - return a reference to the active request
 * @active - the active tracker
 *
 * i915_gem_active_get() returns a reference to the active request, or NULL
 * if the active tracker is idle. The caller must hold struct_mutex.
 */
static inline struct drm_i915_gem_request *
i915_gem_active_get(const struct i915_gem_active *active)
{
	struct drm_i915_gem_request *request;

	request = i915_gem_active_peek(active);
	if (!request || i915_gem_request_completed(request))
		return NULL;

	return i915_gem_request_get(request);
}

/**
 * i915_gem_active_isset - report whether the active tracker is assigned
 * @active - the active tracker
 *
 * i915_gem_active_isset() returns true if the active tracker is currently
 * assigned to a request. Due to the lazy retiring, that request may be idle
 * and this may report stale information.
 */
static inline bool
i915_gem_active_isset(const struct i915_gem_active *active)
{
	return active->request;
}

/**
 * i915_gem_active_is_idle - report whether the active tracker is idle
 * @active - the active tracker
 *
 * i915_gem_active_is_idle() returns true if the active tracker is currently
 * unassigned or if the request is complete (but not yet retired). Requires
 * the caller to hold struct_mutex (but that can be relaxed if desired).
 */
static inline bool
i915_gem_active_is_idle(const struct i915_gem_active *active)
{
	struct drm_i915_gem_request *request;

	request = i915_gem_active_peek(active);
	if (!request || i915_gem_request_completed(request))
		return true;

	return false;
}

/**
 * i915_gem_active_wait - waits until the request is completed
 * @active - the active request on which to wait
 *
 * i915_gem_active_wait() waits until the request is completed before
 * returning. Note that it does not guarantee that the request is
 * retired first, see i915_gem_active_retire().
 */
static inline int __must_check
i915_gem_active_wait(const struct i915_gem_active *active)
{
	struct drm_i915_gem_request *request;

	request = i915_gem_active_peek(active);
	if (!request)
		return 0;

	return i915_wait_request(request);
}

/**
 * i915_gem_active_retire - waits until the request is retired
 * @active - the active request on which to wait
 *
 * i915_gem_active_retire() waits until the request is completed,
 * and then ensures that at least the retirement handler for this
 * @active tracker is called before returning. If the @active
 * tracker is idle, the function returns immediately.
 */
static inline int __must_check
i915_gem_active_retire(const struct i915_gem_active *active)
{
	return i915_gem_active_wait(active);
}

/* Convenience functions for peeking at state inside active's request whilst
 * guarded by the struct_mutex.
 */

static inline uint32_t
i915_gem_active_get_seqno(const struct i915_gem_active *active)
{
	return i915_gem_request_get_seqno(i915_gem_active_peek(active));
}

static inline struct intel_engine_cs *
i915_gem_active_get_engine(const struct i915_gem_active *active)
{
	return i915_gem_request_get_engine(i915_gem_active_peek(active));
}

#define for_each_active(mask, idx) \
	for (; mask ? idx = ffs(mask) - 1, 1 : 0; mask &= ~BIT(idx))

#endif /* I915_GEM_REQUEST_H */
