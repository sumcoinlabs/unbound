/*
 * services/mesh.h - deal with mesh of query states and handle events for that.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains functions to assist in dealing with a mesh of
 * query states. This mesh is supposed to be thread-specific.
 * It consists of query states (per qname, qtype, qclass) and connections
 * between query states and the super and subquery states, and replies to
 * send back to clients.
 */

#ifndef SERVICES_MESH_H
#define SERVICES_MESH_H

#include "util/rbtree.h"
#include "util/netevent.h"
#include "util/data/msgparse.h"
#include "util/module.h"
struct mesh_state;
struct mesh_reply;
struct query_info;
struct reply_info;
struct outbound_entry;
struct timehist;

/** 
 * Mesh of query states
 */
struct mesh_area {
	/** the number of modules */
	int num_modules;
	/** the module callbacks, array of num_modules length (ref only) */
	struct module_func_block** modfunc;
	/** environment for new states */
	struct module_env* env;

	/** set of runnable queries (mesh_state.run_node) */
	rbtree_t run;
	/** rbtree of all current queries (mesh_state.node)*/
	rbtree_t all;

	/** count of the total number of mesh_reply entries */
	size_t num_reply_addrs;
	/** count of the number of mesh_states that have mesh_replies 
	 * Because a state can send results to multiple reply addresses,
	 * this number must be equal or lower than num_reply_addrs. */
	size_t num_reply_states;
	/** number of mesh_states that have no mesh_replies, and also
	 * an empty set of super-states, thus are 'toplevel' or detached
	 * internal opportunistic queries */
	size_t num_detached_states;

	/** number of replies sent */
	size_t replies_sent;
	/** sum of waiting times for the replies */
	struct timeval replies_sum_wait;
	/** histogram of time values */
	struct timehist* histogram;
};

/**
 * A mesh query state
 * Unique per qname, qtype, qclass (from the qstate).
 * And RD / CD flag; in case a client turns it off.
 * And priming queries are different from ordinary queries (because of hints).
 *
 * The entire structure is allocated in a region, this region is the qstate
 * region. All parts (rbtree nodes etc) are also allocated in the region.
 */
struct mesh_state {
	/** node in mesh_area all tree, key is this struct. Must be first. */
	rbnode_t node;
	/** node in mesh_area runnable tree, key is this struct */
	rbnode_t run_node;
	/** the query state. Note that the qinfo and query_flags 
	 * may not change. */
	struct module_qstate s;
	/** the list of replies to clients for the results */
	struct mesh_reply* reply_list;
	/** debug flags */
	int debug_flags;
	/** set of superstates (that want this state's result) 
	 * contains struct mesh_state_ref* */
	rbtree_t super_set;
	/** set of substates (that this state needs to continue)
	 * contains struct mesh_state_ref* */
	rbtree_t sub_set;
};

/**
 * Rbtree reference to a mesh_state.
 * Used in super_set and sub_set. 
 */
struct mesh_state_ref {
	/** node in rbtree for set, key is this structure */
	rbnode_t node;
	/** the mesh state */
	struct mesh_state* s;
};

/**
 * Reply to a client
 */
struct mesh_reply {
	/** next in reply list */
	struct mesh_reply* next;
	/** the query reply destination, packet buffer and where to send. */
	struct comm_reply query_reply;
	/** edns data from query */
	struct edns_data edns;
	/** the time when request was entered */
	struct timeval start_time;
	/** id of query, in network byteorder. */
	uint16_t qid;
	/** flags of query, for reply flags */
	uint16_t qflags;
};

/* ------------------- Functions for worker -------------------- */

/**
 * Allocate mesh, to empty.
 * @param num_modules: number of modules that are present.
 * @param modfunc: array passed (alloced and deleted by caller), that has
 * 	num_modules function callbacks for the modules.
 * @param env: environment for new queries.
 * @return mesh: the new mesh or NULL on error.
 */
struct mesh_area* mesh_create(int num_modules, 
	struct module_func_block** modfunc, struct module_env* env);

/**
 * Delete mesh, and all query states and replies in it.
 * @param mesh: the mesh to delete.
 */
void mesh_delete(struct mesh_area* mesh);

/**
 * New query incoming from clients. Create new query state if needed, and
 * add mesh_reply to it. Returns error to client on malloc failures.
 * Will run the mesh area queries to process if a new query state is created.
 *
 * @param mesh: the mesh.
 * @param qinfo: query from client.
 * @param qflags: flags from client query.
 * @param edns: edns data from client query.
 * @param rep: where to reply to.
 * @param qid: query id to reply with.
 */
void mesh_new_client(struct mesh_area* mesh, struct query_info* qinfo,
	uint16_t qflags, struct edns_data* edns, struct comm_reply* rep, 
	uint16_t qid);

/**
 * Handle new event from the wire. A serviced query has returned.
 * The query state will be made runnable, and the mesh_area will process
 * query states until processing is complete.
 *
 * @param mesh: the query mesh.
 * @param e: outbound entry, with query state to run and reply pointer.
 * @param is_ok: if true, reply is OK, otherwise a timeout happened.
 * @param reply: the comm point reply info.
 */
void mesh_report_reply(struct mesh_area* mesh, struct outbound_entry* e,
	int is_ok, struct comm_reply* reply);

/* ------------------- Functions for module environment --------------- */

/**
 * Detach-subqueries.
 * Remove all sub-query references from this query state.
 * Keeps super-references of those sub-queries correct.
 * Updates stat items in mesh_area structure.
 * @param qstate: used to find mesh state.
 */
void mesh_detach_subs(struct module_qstate* qstate);

/**
 * Attach subquery.
 * Creates it if it does not exist already.
 * Keeps sub and super references correct.
 * Updates stat items in mesh_area structure.
 * Pass if it is priming query or not.
 * return:
 * 	o if error (malloc) happened.
 * 	o need to initialise the new state (module init; it is a new state).
 * 	  so that the next run of the query with this module is successful.
 * 	o no init needed, attachment successful.
 *
 * @param qstate: the state to find mesh state, and that wants to receive
 * 	the results from the new subquery.
 * @param qinfo: what to query for (copied).
 * @param qflags: what flags to use (RD / CD flag or not).
 * @param prime: if it is a (stub) priming query.
 * @param newq: If the new subquery needs initialisation, it is returned,
 * 	otherwise NULL is returned.
 * @return: false on error, true if success (and init may be needed).
 */
int mesh_attach_sub(struct module_qstate* qstate, struct query_info* qinfo,
	uint16_t qflags, int prime, struct module_qstate** newq);

/**
 * Query state is done, send messages to reply entries.
 * Encode messages using reply entry values and the querystate (with original
 * qinfo), using given reply_info.
 * Pass errcode != 0 if an error reply is needed.
 * If no reply entries, nothing is done.
 * Must be called before a module can module_finished or return module_error.
 * The module must handle the super query states itself as well.
 *
 * @param qstate: used for original query info. And to find mesh info.
 * @param rcode: if not 0 (NOERROR) an error is sent back (and rep ignored).
 * @param rep: reply to encode and send back to clients.
 */
void mesh_query_done(struct module_qstate* qstate, int rcode, 
	struct reply_info* rep);

/**
 * Get a callback for the super query states that are interested in the 
 * results from this query state. These can then be changed for error 
 * or results.
 * Must be called befor a module can module_finished or return module_error.
 * After finishing or module error, the super query states become runnable
 * with event module_event_pass.
 *
 * @param qstate: the state that has results, used to find mesh state.
 * @param id: module id.
 * @param cb: callback function. Called as
 * 	cb(qstate, id, super_qstate) for every super query state.
 */
void mesh_walk_supers(struct module_qstate* qstate, int id, 
	void (*cb)(struct module_qstate*, int, struct module_qstate*));

/**
 * Delete mesh state, cleanup and also rbtrees and so on.
 * Will detach from all super/subnodes.
 * @param qstate: to remove.
 */
void mesh_state_delete(struct module_qstate* qstate);

/* ------------------- Functions for mesh -------------------- */

/**
 * Create and initialize a new mesh state and its query state
 * Does not put the mesh state into rbtrees and so on.
 * @param env: module environment to set.
 * @param qinfo: query info that the mesh is for.
 * @param qflags: flags for query (RD / CD flag).
 * @param prime: if true, it is a priming query, set is_priming on mesh state.
 * @return: new mesh state or NULL on allocation error.
 */
struct mesh_state* mesh_state_create(struct module_env* env, 
	struct query_info* qinfo, uint16_t qflags, int prime);

/**
 * Cleanup a mesh state and its query state. Does not do rbtree or 
 * reference cleanup.
 * @param mstate: mesh state to cleanup. Its pointer may no longer be used
 * 	afterwards. Cleanup rbtrees before calling this function.
 */
void mesh_state_cleanup(struct mesh_state* mstate);

/**
 * Find a mesh state in the mesh area. Pass relevant flags.
 *
 * @param mesh: the mesh area to look in.
 * @param qinfo: what query
 * @param qflags: if RD / CD bit is set or not.
 * @param prime: if it is a priming query.
 * @return: mesh state or NULL if not found.
 */
struct mesh_state* mesh_area_find(struct mesh_area* mesh, 
	struct query_info* qinfo, uint16_t qflags, int prime);

/**
 * Setup attachment super/sub relation between super and sub mesh state.
 * The relation must not be present when calling the function.
 * Does not update stat items in mesh_area.
 * @param super: super state.
 * @param sub: sub state.
 * @return: 0 on alloc error.
 */
int mesh_state_attachment(struct mesh_state* super, struct mesh_state* sub);

/**
 * Create new reply structure and attach it to a mesh state.
 * Does not update stat items in mesh area.
 * @param s: the mesh state.
 * @param edns: edns data for reply (bufsize).
 * @param rep: comm point reply info.
 * @param qid: ID of reply.
 * @param qflags: original query flags.
 * @return: 0 on alloc error.
 */
int mesh_state_add_reply(struct mesh_state* s, struct edns_data* edns, 
	struct comm_reply* rep, uint16_t qid, uint16_t qflags);

/**
 * Run the mesh. Run all runnable mesh states. Which can create new
 * runnable mesh states. Until completion. Automatically called by
 * mesh_report_reply and mesh_new_client as needed.
 * @param mesh: mesh area.
 * @param mstate: first mesh state to run.
 * @param ev: event the mstate. Others get event_pass.
 * @param e: if a reply, its outbound entry.
 */
void mesh_run(struct mesh_area* mesh, struct mesh_state* mstate, 
	enum module_ev ev, struct outbound_entry* e);

/**
 * Print some stats about the mesh to the log.
 * @param mesh: the mesh to print it for.
 * @param str: descriptive string to go with it.
 */
void mesh_stats(struct mesh_area* mesh, const char* str);

/**
 * Calculate memory size in use by mesh and all queries inside it.
 * @param mesh: the mesh to examine.
 * @return size in bytes.
 */
size_t mesh_get_mem(struct mesh_area* mesh);

/**
 * Find cycle; see if the given mesh is in the targets sub, or sub-sub, ...
 * trees.
 * @param qstate: given mesh querystate.
 * @param qinfo: query info for dependency.
 * @param flags: query flags of dependency.
 * @param prime: if dependency is a priming query or not.
 * @return true if the name,type,class exists and the given qstate mesh exists
 * 	as a dependency of that name. Thus if qstate becomes dependent on
 * 	name,type,class then a cycle is created.
 */
int mesh_detect_cycle(struct module_qstate* qstate, struct query_info* qinfo,
	uint16_t flags, int prime);

#endif /* SERVICES_MESH_H */
