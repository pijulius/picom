// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui

#include <stddef.h>
#include <uthash.h>
#include <xcb/xproto.h>

#include "common.h"
#include "log.h"
#include "utils/dynarr.h"
#include "utils/list.h"
#include "x.h"

#include "win.h"
#include "wm.h"
#include "wm_internal.h"

struct wm_query_tree_request {
	struct x_async_request_base base;
	struct wm_tree_node *node;
	struct wm *wm;
	struct atom *atoms;
};

struct wm_get_property_request {
	struct x_async_request_base base;
	struct wm *wm;
	xcb_window_t wid;
};

struct wm {
	/// The toplevel currently being focused. Concretely this is the toplevel window
	/// that contains the window currently considered by the X server to be focused.
	struct wm_tree_node *focused_win;
	struct wm_tree tree;

	/// This is a virtual root for all "orphaned" windows. A window is orphaned
	/// if it is not reachable from the root node. This can only be non-empty if
	/// the tree is not consistent, i.e. there are pending async query tree requests.
	///
	/// Note an orphaned window cannot be a toplevel. This is trivially true because
	/// a toplevel has the root window as its parent, and once the root window is
	/// created its list of children is always up to date.
	struct wm_tree_node orphan_root;

	/// Number of pending async query tree requests. We also have async get
	/// property requests, but they are not tracked because they don't
	/// affect the tree structure. We guarantee that when there are pending
	/// query tree requests, no tree nodes will be freed.
	unsigned n_pending_query_trees;

	/// Whether cached window leaders should be recalculated. Following tree changes
	/// will trigger a leader refresh:
	///   - A toplevel is added. This is because a window leader might be set before
	///   we
	///     have imported the target window into our tree.
	///   - A toplevel is removed.
	///   - The leader property of any toplevel is changed. A window's leader changing
	///   affects
	///     all of its descendants in the leadership tree (this is different from the
	///     window tree). But keeping track of the leadership tree is too much work,
	///     so we just refresh all cached leaders.
	bool needs_leader_refresh;
};

// TODO(yshui): this is a bit weird and I am not decided on it yet myself. Maybe we can
// expose `wm_tree_node` directly. But maybe we want to bundle some additional data with
// it. Anyway, this is probably easy to get rid of if we want to.
/// A wrapper of `wm_tree_node`. This points to the `siblings` `struct list_node` in a
/// `struct wm_tree_node`.
struct wm_ref {
	struct list_node inner;
};
static_assert(offsetof(struct wm_ref, inner) == 0, "wm_cursor should be usable as a "
                                                   "wm_tree_node");
static_assert(alignof(struct wm_ref) == alignof(struct list_node),
              "wm_cursor should have the same alignment as wm_tree_node");

static inline const struct wm_tree_node *to_tree_node(const struct wm_ref *cursor) {
	return cursor != NULL ? list_entry(&cursor->inner, struct wm_tree_node, siblings)
	                      : NULL;
}

static inline struct wm_tree_node *to_tree_node_mut(struct wm_ref *cursor) {
	return cursor != NULL ? list_entry(&cursor->inner, struct wm_tree_node, siblings)
	                      : NULL;
}

xcb_window_t wm_ref_win_id(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->id.x;
}

wm_treeid wm_ref_treeid(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->id;
}

struct win *wm_ref_deref(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	if (node->parent == NULL) {
		log_error("Trying to dereference a root node. Expect malfunction.");
		return NULL;
	}
	if (node->parent->parent != NULL) {
		// Don't return the client window if this is not a toplevel node. This
		// saves us from needing to clear `->win` when a window is reparented.
		return NULL;
	}
	return node->win;
}

void wm_ref_set(struct wm_ref *cursor, struct win *w) {
	to_tree_node_mut(cursor)->win = w;
}

void wm_ref_set_focused(struct wm *wm, struct wm_ref *cursor) {
	auto node = to_tree_node_mut(cursor);
	if (wm->focused_win == node) {
		return;
	}

	log_debug("Focused window changed from %#010x to %#010x",
	          wm->focused_win ? wm->focused_win->id.x : 0, node ? node->id.x : 0);
	wm->focused_win = node;
}

void wm_ref_set_leader(struct wm *wm, struct wm_ref *cursor, xcb_window_t leader) {
	// This function only changes `leader`, not the cached `leader_final`. So this has
	// no impact on the returned node of `wm_focused_leader()` - it will be updated
	// along with all `leader_final`s in `wm_refresh_leaders`.
	struct wm_tree_node *node =
	    wm_tree_find_toplevel_for(&wm->tree, to_tree_node_mut(cursor));
	if (node->leader == leader) {
		return;
	}

	wm->needs_leader_refresh = true;
	node->leader = leader;
}

struct wm_ref *wm_focused_win(struct wm *wm) {
	return wm->focused_win ? (struct wm_ref *)&wm->focused_win->siblings : NULL;
}

const struct wm_ref *wm_focused_leader(struct wm *wm) {
	return wm->focused_win != NULL
	           ? (struct wm_ref *)&wm->focused_win->leader_final->siblings
	           : NULL;
}

const struct wm_ref *wm_ref_leader(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	return (struct wm_ref *)&node->leader_final->siblings;
}

bool wm_ref_is_zombie(const struct wm_ref *cursor) {
	return to_tree_node(cursor)->is_zombie;
}

struct wm_ref *wm_ref_below(const struct wm_ref *cursor) {
	return &to_tree_node(cursor)->parent->children != cursor->inner.next
	           ? (struct wm_ref *)cursor->inner.next
	           : NULL;
}

struct wm_ref *wm_ref_above(const struct wm_ref *cursor) {
	return &to_tree_node(cursor)->parent->children != cursor->inner.prev
	           ? (struct wm_ref *)cursor->inner.prev
	           : NULL;
}

struct wm_ref *wm_root_ref(const struct wm *wm) {
	return (struct wm_ref *)&wm->tree.root->siblings;
}

struct wm_ref *wm_ref_topmost_child(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	return !list_is_empty(&node->children) ? (struct wm_ref *)node->children.next : NULL;
}

struct wm_ref *wm_ref_bottommost_child(const struct wm_ref *cursor) {
	auto node = to_tree_node(cursor);
	return !list_is_empty(&node->children) ? (struct wm_ref *)node->children.prev : NULL;
}

struct wm_ref *wm_find(const struct wm *wm, xcb_window_t id) {
	auto node = wm_tree_find(&wm->tree, id);
	return node != NULL ? (struct wm_ref *)&node->siblings : NULL;
}

struct wm_ref *wm_find_by_client(const struct wm *wm, xcb_window_t client) {
	auto node = wm_tree_find(&wm->tree, client);
	if (node == NULL) {
		return NULL;
	}
	auto toplevel = wm_tree_find_toplevel_for(&wm->tree, node);
	return toplevel != NULL ? (struct wm_ref *)&toplevel->siblings : NULL;
}

struct wm_ref *wm_ref_toplevel_of(const struct wm *wm, struct wm_ref *cursor) {
	auto toplevel = wm_tree_find_toplevel_for(&wm->tree, to_tree_node_mut(cursor));
	return toplevel != NULL ? (struct wm_ref *)&toplevel->siblings : NULL;
}

struct wm_ref *wm_ref_client_of(struct wm_ref *cursor) {
	auto client = to_tree_node(cursor)->client_window;
	return client != NULL ? (struct wm_ref *)&client->siblings : NULL;
}

struct wm_ref *wm_stack_end(struct wm *wm) {
	return (struct wm_ref *)&wm->tree.root->children;
}

static struct wm_tree_node *wm_find_leader(struct wm *wm, struct wm_tree_node *node) {
	if (node->leader_final != NULL) {
		if (node->visited) {
			log_warn("Window %#010x is part of a cycle in the leadership "
			         "tree",
			         node->id.x);
		}
		return node->leader_final;
	}

	node->leader_final = node;
	if (node->leader != node->id.x) {
		auto leader_node = wm_tree_find(&wm->tree, node->leader);
		if (leader_node == NULL) {
			return node->leader_final;
		}
		leader_node = wm_tree_find_toplevel_for(&wm->tree, leader_node);
		node->visited = true;
		node->leader_final = wm_find_leader(wm, leader_node);
		node->visited = false;
	}
	return node->leader_final;
}

void wm_refresh_leaders(struct wm *wm) {
	if (!wm->needs_leader_refresh) {
		return;
	}
	wm->needs_leader_refresh = false;
	list_foreach(struct wm_tree_node, i, &wm->tree.root->children, siblings) {
		if (i->is_zombie) {
			// Don't change anything about a zombie window.
			continue;
		}
		i->leader_final = NULL;
	}
	list_foreach(struct wm_tree_node, i, &wm->tree.root->children, siblings) {
		if (i->is_zombie) {
			continue;
		}
		wm_find_leader(wm, i);
	}
}

/// Move window `w` so it's right above `below`, if `below` is 0, `w` is moved
/// to the bottom of the stack
void wm_stack_move_to_above(struct wm *wm, struct wm_ref *cursor, struct wm_ref *below) {
	auto node = to_tree_node_mut(cursor);
	if (node->parent == &wm->orphan_root) {
		// If this window is orphaned, moving it around its siblings is
		// meaningless. Same below.
		return;
	}
	wm_tree_move_to_above(&wm->tree, node, to_tree_node_mut(below));
}

void wm_stack_move_to_end(struct wm *wm, struct wm_ref *cursor, bool to_bottom) {
	auto node = to_tree_node_mut(cursor);
	if (node->parent == &wm->orphan_root) {
		return;
	}
	wm_tree_move_to_end(&wm->tree, node, to_bottom);
}

struct wm *wm_new(void) {
	auto wm = ccalloc(1, struct wm);
	wm_tree_init(&wm->tree);
	list_init_head(&wm->orphan_root.children);
	wm->n_pending_query_trees = 0;
	return wm;
}

void wm_free(struct wm *wm) {
	// Free all `struct win`s associated with tree nodes, this leaves dangling
	// pointers, but we are freeing the tree nodes immediately after, so everything
	// is fine (TM).
	if (wm->tree.root != NULL) {
		wm_stack_foreach_safe(wm, i, next) {
			auto w = wm_ref_deref(i);
			auto tree_node = to_tree_node_mut(i);
			free(w);

			if (tree_node->is_zombie) {
				// This mainly happens on `session_destroy`, e.g. when
				// there's ongoing animations.
				log_debug("Leftover zombie node for window %#010x",
				          tree_node->id.x);
				wm_tree_reap_zombie(tree_node);
			}
		}
	}
	wm_tree_clear(&wm->tree);
	assert(wm_is_consistent(wm));
	assert(list_is_empty(&wm->orphan_root.children));

	free(wm);
}

/// Once the window tree reaches a consistent state, we know any tree nodes that are not
/// reachable from the root must have been destroyed, so we can safely free them.
///
/// There are cases where we won't receive DestroyNotify events for these windows. For
/// example, if a window is reparented to a window that is not yet in our tree, then
/// destroyed, we won't receive a DestroyNotify event for it.
static void wm_reap_orphans(struct wm *wm) {
	// Reap orphaned windows
	while (!list_is_empty(&wm->orphan_root.children)) {
		auto node =
		    list_entry(wm->orphan_root.children.next, struct wm_tree_node, siblings);
		list_remove(&node->siblings);
		if (!list_is_empty(&node->children)) {
			log_error("Orphaned window %#010x still has children", node->id.x);
			list_foreach(struct wm_tree_node, i, &node->children, siblings) {
				log_error("  Child: %#010x", i->id.x);
			}
			list_splice(&node->children, &wm->orphan_root.children);
		}
		log_debug("Reaped orphaned window %#010x", node->id.x);
		HASH_DEL(wm->tree.nodes, node);
		free(node);
	}
}

/// Move `from->win` to `to->win`, update `win->tree_ref`.
static void wm_move_win(struct wm_tree_node *from, struct wm_tree_node *to) {
	if (from->win != NULL) {
		from->win->tree_ref = (struct wm_ref *)&to->siblings;
	}
	to->win = from->win;
	from->win = NULL;
}

void wm_destroy(struct wm *wm, xcb_window_t wid) {
	struct wm_tree_node *node = wm_tree_find(&wm->tree, wid);
	if (!node) {
		if (wm_is_consistent(wm)) {
			log_error("Window %#010x destroyed, but it's not in our tree.", wid);
		}
		return;
	}

	log_debug("Destroying window %#010x", wid);

	if (!list_is_empty(&node->children)) {
		log_error("Window %#010x is destroyed but it still has children. "
		          "Orphaning them.",
		          wid);
		list_foreach(struct wm_tree_node, i, &node->children, siblings) {
			log_error("  Child: %#010x", i->id.x);
		}
		list_splice(&node->children, &wm->orphan_root.children);
	}

	if (node == wm->focused_win) {
		wm->focused_win = NULL;
	}

	auto zombie = wm_tree_detach(&wm->tree, node);
	assert(zombie != NULL || node->win == NULL);
	if (zombie != NULL) {
		wm_move_win(node, zombie);
	}

	if (node->req != NULL) {
		// Effectively "cancel" the query tree request by setting `node->req` to
		// NULL. This will cause its reply to be ignored.
		node->req->node = NULL;
		wm->n_pending_query_trees--;
	}
	HASH_DEL(wm->tree.nodes, node);
	free(node);
}

void wm_reap_zombie(struct wm_ref *zombie) {
	wm_tree_reap_zombie(to_tree_node_mut(zombie));
}

struct wm_wid_or_node {
	union {
		xcb_window_t wid;
		struct wm_tree_node *node;
	};
	bool is_wid;
};

/// Start the import process of `wid`. If `new` is not NULL, it means the window is
/// reusing the same window ID as a previously destroyed window, and that destroyed window
/// is in our orphan tree. In this case, we revive the orphaned window instead of creating
/// a new one.
static void
wm_new_or_attach_window(struct wm *wm, struct x_connection *c, struct atom *atoms,
                        struct wm_wid_or_node window, struct wm_tree_node *parent);

void wm_reparent(struct wm *wm, struct x_connection *c, struct atom *atoms,
                 xcb_window_t wid, xcb_window_t parent) {
	auto window = wm_tree_find(&wm->tree, wid);
	auto new_parent = wm_tree_find(&wm->tree, parent);
	bool new_parent_imported = new_parent != NULL && new_parent->req == NULL;

	if (window && window->parent == new_parent) {
		// Reparent to the same parent moves the window to the top of the
		// stack
		BUG_ON(!new_parent_imported);
		wm_tree_move_to_end(&wm->tree, window, false);
		return;
	}

	if (window != NULL) {
		log_debug("Detaching window %#010x from window %#010x", wid,
		          window->parent->id.x);
		auto zombie = wm_tree_detach(&wm->tree, window);
		assert(zombie != NULL || window->win == NULL);
		if (zombie != NULL) {
			wm_move_win(window, zombie);
		}
	}

	// Attaching `window` to `new_parent` will change the children list of
	// `new_parent`, if there is a pending query tree request for `new_parent`, doing
	// so will create an overlap. In other words, `window` will appear in the query
	// tree reply too. Generally speaking, we want to keep a node's children list
	// empty while there is a pending query tree request for it. (Imagine sending the
	// query tree "locks" the children list until the reply is processed). Same logic
	// applies to `wm_import_start`.
	//
	// Alternatively if the new parent isn't in our tree yet, we orphan the window
	// too. Or if we have an orphaned window indicating the new parent was reusing
	// a destroyed window's ID, then we know we will re-query the new parent later
	// when we encounter it in a query tree reply, so we orphan the window in this
	// case as well.
	if (!new_parent_imported) {
		if (window != NULL) {
			log_debug("Window %#010x is attached to window %#010x which is "
			          "currently been queried, orphaning.",
			          window->id.x, parent);
			wm_tree_attach(&wm->tree, window, &wm->orphan_root);
		}
		return;
	}

	struct wm_wid_or_node wid_or_window = {
	    .is_wid = window == NULL,
	};
	if (window == NULL) {
		if (wm_is_consistent(wm)) {
			log_error("Window %#010x reparented, but it's not in "
			          "our tree.",
			          wid);
		}
		wid_or_window.wid = wid;
	} else {
		wid_or_window.node = window;
	}

	/// If a previously unseen window (window reusing a destroyed window's ID is
	/// considered unseen as well) is reparented to a window that has been fully
	/// imported, we must treat it as a newly created window. Because it will not be
	/// included in a query tree reply, so we must initiate its import process
	/// explicitly. `wm_new_or_attach_window` will handle this appropriately.
	log_debug("Reparented window %#010x to window %#010x", window->id.x, parent);
	wm_new_or_attach_window(wm, c, atoms, wid_or_window, new_parent);
}

void wm_set_has_wm_state(struct wm *wm, struct wm_ref *cursor, bool has_wm_state) {
	wm_tree_set_wm_state(&wm->tree, to_tree_node_mut(cursor), has_wm_state);
}

static const xcb_event_mask_t WM_IMPORT_EV_MASK =
    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;

static void
wm_handle_query_tree_reply(struct x_connection *c, struct x_async_request_base *base,
                           xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct wm_query_tree_request *)base;
	auto wm = req->wm;

	auto node = req->node;

	if (node == NULL) {
		// If the node was previously destroyed, then it means a newly created
		// window has reused its window ID. We should ignore the query tree reply,
		// because we haven't setup event mask for this window yet, we won't be
		// able to stay up-to-date with this window, and would have to re-query in
		// `wm_import_start_no_flush` anyway. And we don't have a node to attach
		// the children to anyway.
		if (reply_or_error->response_type != 0) {
			log_debug("Ignoring query tree reply for unknown window, it has "
			          "been destroyed.");
		}
		goto out;
	}

	wm->n_pending_query_trees--;

	if (reply_or_error == NULL) {
		goto out;
	}

	if (reply_or_error->response_type == 0) {
		// This is an error, most likely the window is gone when we tried
		// to query it.
		xcb_generic_error_t *err = (xcb_generic_error_t *)reply_or_error;
		log_debug("Query tree request for window %#010x failed with "
		          "error %s",
		          node == NULL ? 0 : node->id.x, x_strerror(err));
		goto out;
	}

	BUG_ON(node->req != req);
	node->req = NULL;

	xcb_query_tree_reply_t *reply = (xcb_query_tree_reply_t *)reply_or_error;
	log_debug("Finished querying tree for window %#010x", node->id.x);

	auto children = xcb_query_tree_children(reply);
	log_debug("Window %#010x has %d children", node->id.x,
	          xcb_query_tree_children_length(reply));
	for (int i = 0; i < xcb_query_tree_children_length(reply); i++) {
		auto child = children[i];
		wm_new_or_attach_window(
		    wm, c, req->atoms,
		    (struct wm_wid_or_node){.is_wid = true, .wid = child}, node);
	}
	x_flush(c);        // Actually send the requests

out:
	free(req);
	if (wm_is_consistent(wm)) {
		wm_reap_orphans(wm);
	}
}

static void wm_handle_get_wm_state_reply(struct x_connection * /*c*/,
                                         struct x_async_request_base *base,
                                         xcb_raw_generic_event_t *reply_or_error) {
	auto req = (struct wm_get_property_request *)base;
	if (reply_or_error == NULL) {
		free(req);
		return;
	}

	// We guarantee that if a query tree request is pending, its corresponding
	// window tree node won't be reaped. But we don't guarantee the same for
	// get property requests. So we need to search the node by window ID again.

	if (reply_or_error->response_type == 0) {
		// This is an error, most likely the window is gone when we tried
		// to query it. (Note the tree node might have been freed at this
		// point if the query tree request also failed earlier.)
		xcb_generic_error_t *err = (xcb_generic_error_t *)reply_or_error;
		log_debug("Get WM_STATE request for window %#010x failed with "
		          "error %s",
		          req->wid, x_strerror(err));
		free(req);
		return;
	}

	auto node = wm_tree_find(&req->wm->tree, req->wid);
	BUG_ON_NULL(node);        // window must exist at this point, but it might be
	                          // freed then recreated while we were waiting for the
	                          // reply.
	auto reply = (xcb_get_property_reply_t *)reply_or_error;
	wm_tree_set_wm_state(&req->wm->tree, node, reply->type != XCB_NONE);
	free(req);
}

/// Create a window for `wid`, or if an orphan with the same ID exists, re-attach it to
/// the tree at `parent`. Send query tree and get property requests for the window if it
/// is possible that it is a new window. An orphan could actually be a completely
/// different window when it comes back, because we don't receive destroy events for
/// orphans.
///
/// Note this function does not flush the X connection.
static void
wm_new_or_attach_window(struct wm *wm, struct x_connection *c, struct atom *atoms,
                        struct wm_wid_or_node window, struct wm_tree_node *parent) {
	// Try to see there is a window with an unknown parent with the same ID. If so, we
	// now know who its parent is, and we just need to attach it.
	auto new = window.is_wid ? wm_tree_find(&wm->tree, window.wid) : window.node;
	assert(new == NULL || new->parent == &wm->orphan_root);
	if (new == NULL) {
		BUG_ON(!window.is_wid);
		new = wm_tree_new_window(&wm->tree, window.wid);
		wm_tree_add_window(&wm->tree, new);
	} else {
		if (new->parent == parent) {
			// What's going on???
			log_error("Importing window %#010x a second time", new->id.x);
			assert(false);
			return;
		}
		if (new->parent != &wm->orphan_root) {
			log_error("Window %#010x appeared in the children list of both "
			          "%#010x (previous) and %#010x (current).",
			          new->id.x, new->parent->id.x, parent->id.x);
			assert(false);
		}
		log_debug("Re-attaching orphaned window %#010x to window %#010x",
		          new->id.x, parent->id.x);
		auto zombie = wm_tree_detach(&wm->tree, new);
		if (zombie != NULL) {
			// This only happens if `new` is not orphaned, which means things
			// are already going wrong.
			wm_tree_reap_zombie(zombie);
		}
	}
	wm_tree_attach(&wm->tree, new, parent);

	if (!list_is_empty(&new->children)) {
		// If new is an orphan that has children, it means it must have been a
		// window that we have imported. Because it either retained some of its
		// children, and a window that has children cannot be destroyed; or it has
		// acquired new children via create/reparent events, which means we have
		// set up event mask for it. In this case, we don't import it again.
		//
		// If it doesn't have children, it is possible that this `new` we see here
		// is a completely different window just reusing the same window ID as the
		// orphan. This is because orphans have parents not yet imported, we won't
		// receive destroy events for them, so we won't know when they are
		// destroyed. In this case, we treat it as a new window (even though it
		// might not be).
		return;
	}

	log_debug("Starting import process for window %#010x", new->id.x);
	x_change_window_attributes(c, new->id.x, XCB_CW_EVENT_MASK,
	                           (const uint32_t[]){WM_IMPORT_EV_MASK},
	                           PENDING_REPLY_ACTION_IGNORE);

	if (new->req != NULL) {
		// If there is already a query tree request in-flight, cancel it.
		new->req->node = NULL;
		wm->n_pending_query_trees--;
	}

	{
		auto req = ccalloc(1, struct wm_query_tree_request);
		req->base.callback = wm_handle_query_tree_reply;
		req->node = new;
		req->wm = wm;
		req->atoms = atoms;
		new->req = req;
		wm->n_pending_query_trees++;
		x_async_query_tree(c, new->id.x, &req->base);
	}

	// (It's OK to resend the get property request even if one is already in-flight,
	// unlike query tree.)
	{
		auto req = ccalloc(1, struct wm_get_property_request);
		req->base.callback = wm_handle_get_wm_state_reply;
		req->wm = wm;
		req->wid = new->id.x;
		x_async_get_property(c, new->id.x, atoms->aWM_STATE, XCB_ATOM_ANY, 0, 2,
		                     &req->base);
	}
}

void wm_import_start(struct wm *wm, struct x_connection *c, struct atom *atoms,
                     xcb_window_t wid, struct wm_ref *parent) {
	struct wm_tree_node *parent_node = parent != NULL ? to_tree_node_mut(parent) : NULL;
	if (parent_node != NULL && parent_node->req != NULL) {
		// Parent node is currently being queried, we can't attach the new window
		// to it as that will change its children list.
		return;
	}

	wm_new_or_attach_window(
	    wm, c, atoms, (struct wm_wid_or_node){.is_wid = true, .wid = wid}, parent_node);
	x_flush(c);        // Actually send the requests
}

bool wm_is_consistent(const struct wm *wm) {
	return wm->n_pending_query_trees == 0;
}

bool wm_has_tree_changes(const struct wm *wm) {
	return !list_is_empty(&wm->tree.changes);
}

struct wm_change wm_dequeue_change(struct wm *wm) {
	auto tree_change = wm_tree_dequeue_change(&wm->tree);
	struct wm_change ret = {
	    .type = tree_change.type,
	    .toplevel = NULL,
	};
	switch (tree_change.type) {
	case WM_TREE_CHANGE_CLIENT:
		ret.client.old = tree_change.client.old;
		ret.client.new_ = tree_change.client.new_;
		ret.toplevel = (struct wm_ref *)&tree_change.client.toplevel->siblings;
		break;
	case WM_TREE_CHANGE_TOPLEVEL_KILLED:
		ret.toplevel = (struct wm_ref *)&tree_change.killed->siblings;
		wm->needs_leader_refresh = true;
		break;
	case WM_TREE_CHANGE_TOPLEVEL_NEW:
		ret.toplevel = (struct wm_ref *)&tree_change.new_->siblings;
		wm->needs_leader_refresh = true;
		break;
	default: break;
	}
	return ret;
}

struct wm_ref *wm_new_mock_window(struct wm *wm, xcb_window_t wid) {
	auto node = wm_tree_new_window(&wm->tree, wid);
	return (struct wm_ref *)&node->siblings;
}
void wm_free_mock_window(struct wm * /*wm*/, struct wm_ref *cursor) {
	free(to_tree_node_mut(cursor));
}
