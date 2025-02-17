/*
   Unix SMB/CIFS implementation.
   Infrastructure for async requests
   Copyright (C) Volker Lendecke 2008
   Copyright (C) Stefan Metzmacher 2009

     ** NOTE! The following LGPL license applies to the tevent
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "tevent.h"
#include "tevent_internal.h"
#include "tevent_util.h"

char *tevent_req_default_print(struct tevent_req *req, TALLOC_CTX *mem_ctx)
{
	return talloc_asprintf(mem_ctx,
			       "tevent_req[%p/%s]: state[%d] error[%lld (0x%llX)] "
			       " state[%s (%p)] timer[%p] finish[%s]",
			       req, req->internal.create_location,
			       req->internal.state,
			       (unsigned long long)req->internal.error,
			       (unsigned long long)req->internal.error,
			       req->internal.private_type,
			       req->data,
			       req->internal.timer,
			       req->internal.finish_location
			       );
}

char *tevent_req_print(TALLOC_CTX *mem_ctx, struct tevent_req *req)
{
	if (req == NULL) {
		return talloc_strdup(mem_ctx, "tevent_req[NULL]");
	}

	if (!req->private_print) {
		return tevent_req_default_print(req, mem_ctx);
	}

	return req->private_print(req, mem_ctx);
}

static int tevent_req_destructor(struct tevent_req *req);

struct tevent_req *_tevent_req_create(TALLOC_CTX *mem_ctx,
				    void *pdata,
				    size_t data_size,
				    const char *type,
				    const char *location)
{
	struct tevent_req *req;
	struct tevent_req *parent;
	void **ppdata = (void **)pdata;
	void *data;
	size_t payload;

	payload = sizeof(struct tevent_immediate) + data_size;
	if (payload < sizeof(struct tevent_immediate)) {
		/* overflow */
		return NULL;
	}

	req = talloc_pooled_object(
		mem_ctx, struct tevent_req, 2,
		sizeof(struct tevent_immediate) + data_size);
	if (req == NULL) {
		return NULL;
	}

	*req = (struct tevent_req) {
		.internal = {
			.private_type		= type,
			.create_location	= location,
			.state			= TEVENT_REQ_IN_PROGRESS,
			.trigger		= tevent_create_immediate(req),
		},
	};

	data = talloc_zero_size(req, data_size);

	/*
	 * No need to check for req->internal.trigger!=NULL or
	 * data!=NULL, this can't fail: talloc_pooled_object has
	 * already allocated sufficient memory.
	 */

	talloc_set_name_const(data, type);

	req->data = data;

	talloc_set_destructor(req, tevent_req_destructor);

	parent = talloc_get_type(talloc_parent(mem_ctx), struct tevent_req);
	if ((parent != NULL) && (parent->internal.profile != NULL)) {
		bool ok = tevent_req_set_profile(req);

		if (!ok) {
			TALLOC_FREE(req);
			return NULL;
		}
		req->internal.profile->parent = parent->internal.profile;
		DLIST_ADD_END(parent->internal.profile->subprofiles,
			      req->internal.profile);
	}

	*ppdata = data;

	/* Initially, talloc_zero_size() sets internal.call_depth to 0 */
	if (parent != NULL && parent->internal.call_depth > 0) {
		req->internal.call_depth = parent->internal.call_depth + 1;
		tevent_thread_call_depth_set(req->internal.call_depth);
	}

	return req;
}

static int tevent_req_destructor(struct tevent_req *req)
{
	tevent_req_received(req);
	return 0;
}

void _tevent_req_notify_callback(struct tevent_req *req, const char *location)
{
	req->internal.finish_location = location;
	if (req->internal.defer_callback_ev) {
		(void)tevent_req_post(req, req->internal.defer_callback_ev);
		req->internal.defer_callback_ev = NULL;
		return;
	}
	if (req->async.fn != NULL) {
		/* Calling back the parent code, decrement the call depth. */
		tevent_thread_call_depth_set(req->internal.call_depth > 0 ?
					     req->internal.call_depth - 1 : 0);
		req->async.fn(req);
	}
}

static void tevent_req_cleanup(struct tevent_req *req)
{
	if (req->private_cleanup.fn == NULL) {
		return;
	}

	if (req->private_cleanup.state >= req->internal.state) {
		/*
		 * Don't call the cleanup_function multiple times for the same
		 * state recursively
		 */
		return;
	}

	req->private_cleanup.state = req->internal.state;
	req->private_cleanup.fn(req, req->internal.state);
}

static void tevent_req_finish(struct tevent_req *req,
			      enum tevent_req_state state,
			      const char *location)
{
	struct tevent_req_profile *p;
	/*
	 * make sure we do not timeout after
	 * the request was already finished
	 */
	TALLOC_FREE(req->internal.timer);

	req->internal.state = state;
	req->internal.finish_location = location;

	tevent_req_cleanup(req);

	p = req->internal.profile;

	if (p != NULL) {
		p->stop_location = location;
		p->stop_time = tevent_timeval_current();
		p->state = state;
		p->user_error = req->internal.error;

		if (p->parent != NULL) {
			talloc_steal(p->parent, p);
			req->internal.profile = NULL;
		}
	}

	_tevent_req_notify_callback(req, location);
}

void _tevent_req_done(struct tevent_req *req,
		      const char *location)
{
	tevent_req_finish(req, TEVENT_REQ_DONE, location);
}

bool _tevent_req_error(struct tevent_req *req,
		       uint64_t error,
		       const char *location)
{
	if (error == 0) {
		return false;
	}

	req->internal.error = error;
	tevent_req_finish(req, TEVENT_REQ_USER_ERROR, location);
	return true;
}

void _tevent_req_oom(struct tevent_req *req, const char *location)
{
	tevent_req_finish(req, TEVENT_REQ_NO_MEMORY, location);
}

bool _tevent_req_nomem(const void *p,
		       struct tevent_req *req,
		       const char *location)
{
	if (p != NULL) {
		return false;
	}
	_tevent_req_oom(req, location);
	return true;
}

/**
 * @internal
 *
 * @brief Immediate event callback.
 *
 * @param[in]  ev       The event context to use.
 *
 * @param[in]  im       The immediate event.
 *
 * @param[in]  priv     The async request to be finished.
 */
static void tevent_req_trigger(struct tevent_context *ev,
			       struct tevent_immediate *im,
			       void *private_data)
{
	struct tevent_req *req =
		talloc_get_type_abort(private_data,
		struct tevent_req);

	tevent_req_finish(req, req->internal.state,
			  req->internal.finish_location);
}

struct tevent_req *tevent_req_post(struct tevent_req *req,
				   struct tevent_context *ev)
{
	tevent_schedule_immediate(req->internal.trigger,
				  ev, tevent_req_trigger, req);
	return req;
}

void tevent_req_defer_callback(struct tevent_req *req,
			       struct tevent_context *ev)
{
	req->internal.defer_callback_ev = ev;
}

bool tevent_req_is_in_progress(struct tevent_req *req)
{
	if (req->internal.state == TEVENT_REQ_IN_PROGRESS) {
		return true;
	}

	return false;
}

void tevent_req_received(struct tevent_req *req)
{
	talloc_set_destructor(req, NULL);

	req->private_print = NULL;
	req->private_cancel = NULL;

	TALLOC_FREE(req->internal.trigger);
	TALLOC_FREE(req->internal.timer);

	req->internal.state = TEVENT_REQ_RECEIVED;

	tevent_req_cleanup(req);

	TALLOC_FREE(req->data);
}

bool tevent_req_poll(struct tevent_req *req,
		     struct tevent_context *ev)
{
	while (tevent_req_is_in_progress(req)) {
		int ret;

		ret = tevent_loop_once(ev);
		if (ret != 0) {
			return false;
		}
	}

	return true;
}

bool tevent_req_is_error(struct tevent_req *req, enum tevent_req_state *state,
			uint64_t *error)
{
	if (req->internal.state == TEVENT_REQ_DONE) {
		return false;
	}
	if (req->internal.state == TEVENT_REQ_USER_ERROR) {
		*error = req->internal.error;
	}
	*state = req->internal.state;
	return true;
}

static void tevent_req_timedout(struct tevent_context *ev,
			       struct tevent_timer *te,
			       struct timeval now,
			       void *private_data)
{
	struct tevent_req *req =
		talloc_get_type_abort(private_data,
		struct tevent_req);

	TALLOC_FREE(req->internal.timer);

	tevent_req_finish(req, TEVENT_REQ_TIMED_OUT, __FUNCTION__);
}

bool tevent_req_set_endtime(struct tevent_req *req,
			    struct tevent_context *ev,
			    struct timeval endtime)
{
	TALLOC_FREE(req->internal.timer);

	req->internal.timer = tevent_add_timer(ev, req, endtime,
					       tevent_req_timedout,
					       req);
	if (tevent_req_nomem(req->internal.timer, req)) {
		return false;
	}

	return true;
}

void tevent_req_reset_endtime(struct tevent_req *req)
{
	TALLOC_FREE(req->internal.timer);
}

void tevent_req_set_callback(struct tevent_req *req, tevent_req_fn fn, void *pvt)
{
	req->async.fn = fn;
	req->async.private_data = pvt;
}

void *_tevent_req_callback_data(struct tevent_req *req)
{
	return req->async.private_data;
}

void *_tevent_req_data(struct tevent_req *req)
{
	return req->data;
}

void tevent_req_set_print_fn(struct tevent_req *req, tevent_req_print_fn fn)
{
	req->private_print = fn;
}

void tevent_req_set_cancel_fn(struct tevent_req *req, tevent_req_cancel_fn fn)
{
	req->private_cancel = fn;
}

bool _tevent_req_cancel(struct tevent_req *req, const char *location)
{
	if (req->private_cancel == NULL) {
		return false;
	}

	return req->private_cancel(req);
}

void tevent_req_set_cleanup_fn(struct tevent_req *req, tevent_req_cleanup_fn fn)
{
	req->private_cleanup.state = req->internal.state;
	req->private_cleanup.fn = fn;
}

static int tevent_req_profile_destructor(struct tevent_req_profile *p);

bool tevent_req_set_profile(struct tevent_req *req)
{
	struct tevent_req_profile *p;

	if (req->internal.profile != NULL) {
		tevent_req_error(req, EINVAL);
		return false;
	}

	p = tevent_req_profile_create(req);

	if (tevent_req_nomem(p, req)) {
		return false;
	}

	p->req_name = talloc_get_name(req->data);
	p->start_location = req->internal.create_location;
	p->start_time = tevent_timeval_current();

	req->internal.profile = p;

	return true;
}

static int tevent_req_profile_destructor(struct tevent_req_profile *p)
{
	if (p->parent != NULL) {
		DLIST_REMOVE(p->parent->subprofiles, p);
		p->parent = NULL;
	}

	while (p->subprofiles != NULL) {
		p->subprofiles->parent = NULL;
		DLIST_REMOVE(p->subprofiles, p->subprofiles);
	}

	return 0;
}

struct tevent_req_profile *tevent_req_move_profile(struct tevent_req *req,
						   TALLOC_CTX *mem_ctx)
{
	return talloc_move(mem_ctx, &req->internal.profile);
}

const struct tevent_req_profile *tevent_req_get_profile(
	struct tevent_req *req)
{
	return req->internal.profile;
}

void tevent_req_profile_get_name(const struct tevent_req_profile *profile,
				 const char **req_name)
{
	if (req_name != NULL) {
		*req_name = profile->req_name;
	}
}

void tevent_req_profile_get_start(const struct tevent_req_profile *profile,
				  const char **start_location,
				  struct timeval *start_time)
{
	if (start_location != NULL) {
		*start_location = profile->start_location;
	}
	if (start_time != NULL) {
		*start_time = profile->start_time;
	}
}

void tevent_req_profile_get_stop(const struct tevent_req_profile *profile,
				 const char **stop_location,
				 struct timeval *stop_time)
{
	if (stop_location != NULL) {
		*stop_location = profile->stop_location;
	}
	if (stop_time != NULL) {
		*stop_time = profile->stop_time;
	}
}

void tevent_req_profile_get_status(const struct tevent_req_profile *profile,
				   pid_t *pid,
				   enum tevent_req_state *state,
				   uint64_t *user_error)
{
	if (pid != NULL) {
		*pid = profile->pid;
	}
	if (state != NULL) {
		*state = profile->state;
	}
	if (user_error != NULL) {
		*user_error = profile->user_error;
	}
}

const struct tevent_req_profile *tevent_req_profile_get_subprofiles(
	const struct tevent_req_profile *profile)
{
	return profile->subprofiles;
}

const struct tevent_req_profile *tevent_req_profile_next(
	const struct tevent_req_profile *profile)
{
	return profile->next;
}

struct tevent_req_profile *tevent_req_profile_create(TALLOC_CTX *mem_ctx)
{
	struct tevent_req_profile *result;

	result = talloc_zero(mem_ctx, struct tevent_req_profile);
	if (result == NULL) {
		return NULL;
	}
	talloc_set_destructor(result, tevent_req_profile_destructor);

	return result;
}

bool tevent_req_profile_set_name(struct tevent_req_profile *profile,
				 const char *req_name)
{
	profile->req_name = talloc_strdup(profile, req_name);
	return (profile->req_name != NULL);
}

bool tevent_req_profile_set_start(struct tevent_req_profile *profile,
				  const char *start_location,
				  struct timeval start_time)
{
	profile->start_time = start_time;

	profile->start_location = talloc_strdup(profile, start_location);
	return (profile->start_location != NULL);
}

bool tevent_req_profile_set_stop(struct tevent_req_profile *profile,
				 const char *stop_location,
				 struct timeval stop_time)
{
	profile->stop_time = stop_time;

	profile->stop_location = talloc_strdup(profile, stop_location);
	return (profile->stop_location != NULL);
}

void tevent_req_profile_set_status(struct tevent_req_profile *profile,
				   pid_t pid,
				   enum tevent_req_state state,
				   uint64_t user_error)
{
	profile->pid = pid;
	profile->state = state;
	profile->user_error = user_error;
}

void tevent_req_profile_append_sub(struct tevent_req_profile *parent_profile,
				   struct tevent_req_profile **sub_profile)
{
	struct tevent_req_profile *sub;

	sub = talloc_move(parent_profile, sub_profile);

	sub->parent = parent_profile;
	DLIST_ADD_END(parent_profile->subprofiles, sub);
}
