/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Some portions of this software is derived from the
 * https://github.com/halayli/lthread which carrys the following license.
 *
 * Copyright (C) 2012, Hasan Alayli <halayli@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#define RTE_MEM 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sched.h>

#include <rte_prefetch.h>
#include <rte_per_lcore.h>
#include <rte_atomic.h>
#include <rte_atomic_64.h>
#include <rte_log.h>
#include <rte_common.h>
#include <rte_branch_prediction.h>

#include "nf_lthread_api.h"
#include "nf_lthread_int.h"
#include "nf_lthread_sched.h"
#include "core_manager.h"
#include "lthread_objcache.h"
#include "lthread_timer.h"
#include "lthread_mutex.h"
#include "lthread_cond.h"
#include "lthread_tls.h"
#include "lthread_diag.h"

/*
 * This file implements the lthread scheduler
 * The scheduler is the function lthread_run()
 * This must be run as the main loop of an EAL thread.
 *
 * Currently once a scheduler is created it cannot be destroyed
 * When a scheduler shuts down it is assumed that the application is terminating
 */
//全局变量都要设为原子变量，原子操作
static rte_atomic16_t num_schedulers;
static rte_atomic16_t active_schedulers;


/* one scheduler per lcore */
RTE_DEFINE_PER_LCORE(struct lthread_sched *, this_sched) = NULL;
RTE_DEFINE_PER_LCORE(int, counter) = 0;
#define COUNTER RTE_PER_LCORE(counter)

struct lthread_sched *schedcore[LTHREAD_MAX_LCORES];
static try_init_Agent = 0;
static init_Agent_suc = 0;

diag_callback diag_cb;

uint64_t diag_mask;


/* constructor */
void lthread_sched_ctor(void) __attribute__ ((constructor));
void lthread_sched_ctor(void)
{
	memset(schedcore, 0, sizeof(schedcore));
	try_init_Agent = 0;
	init_Agent_suc = 0;
	rte_atomic16_init(&num_schedulers);
	rte_atomic16_set(&num_schedulers, 1);
	rte_atomic16_init(&active_schedulers);
	rte_atomic16_set(&active_schedulers, 0);
	rte_atomic16_init(&num_nf_threads);
	rte_atomic16_set(&num_nf_threads, 0);

	diag_cb = NULL;
}


enum sched_alloc_phase {
	SCHED_ALLOC_OK,
	SCHED_ALLOC_QNODE_POOL,
	SCHED_ALLOC_READY_QUEUE,
	SCHED_ALLOC_PREADY_QUEUE,
	SCHED_ALLOC_LTHREAD_CACHE,
	SCHED_ALLOC_STACK_CACHE,
	SCHED_ALLOC_PERLT_CACHE,
	SCHED_ALLOC_TLS_CACHE,
	SCHED_ALLOC_COND_CACHE,
	SCHED_ALLOC_MUTEX_CACHE,
};

static int
_lthread_sched_alloc_resources(struct lthread_sched *new_sched)
{
	int alloc_status;

	do {
		/* Initialize per scheduler queue node pool */
		alloc_status = SCHED_ALLOC_QNODE_POOL;
		new_sched->qnode_pool =
			_qnode_pool_create("qnode pool", LTHREAD_PREALLOC);
		if (new_sched->qnode_pool == NULL)
			break;

		/* Initialize per scheduler local ready queue */
		alloc_status = SCHED_ALLOC_READY_QUEUE;
		new_sched->ready = _lthread_queue_create("ready queue");
		if (new_sched->ready == NULL)
			break;

		/* Initialize per scheduler local peer ready queue */
		alloc_status = SCHED_ALLOC_PREADY_QUEUE;
		new_sched->pready = _lthread_queue_create("pready queue");
		if (new_sched->pready == NULL)
			break;

		/* Initialize per scheduler local free lthread cache */
		alloc_status = SCHED_ALLOC_LTHREAD_CACHE;
		new_sched->lthread_cache =
			_lthread_objcache_create("lthread cache",
						sizeof(struct lthread),
						LTHREAD_PREALLOC);
		if (new_sched->lthread_cache == NULL)
			break;

		/* Initialize per scheduler local free stack cache */
		alloc_status = SCHED_ALLOC_STACK_CACHE;
		new_sched->stack_cache =
			_lthread_objcache_create("stack_cache",
						sizeof(struct lthread_stack),
						LTHREAD_PREALLOC);
		if (new_sched->stack_cache == NULL)
			break;

		/* Initialize per scheduler local free per lthread data cache */
		alloc_status = SCHED_ALLOC_PERLT_CACHE;
		new_sched->per_lthread_cache =
			_lthread_objcache_create("per_lt cache",
						RTE_PER_LTHREAD_SECTION_SIZE,
						LTHREAD_PREALLOC);
		if (new_sched->per_lthread_cache == NULL)
			break;

		/* Initialize per scheduler local free tls cache */
		alloc_status = SCHED_ALLOC_TLS_CACHE;
		new_sched->tls_cache =
			_lthread_objcache_create("TLS cache",
						sizeof(struct lthread_tls),
						LTHREAD_PREALLOC);
		if (new_sched->tls_cache == NULL)
			break;

		/* Initialize per scheduler local free cond var cache */
		alloc_status = SCHED_ALLOC_COND_CACHE;
		new_sched->cond_cache =
			_lthread_objcache_create("cond cache",
						sizeof(struct lthread_cond),
						LTHREAD_PREALLOC);
		if (new_sched->cond_cache == NULL)
			break;

		/* Initialize per scheduler local free mutex cache */
		alloc_status = SCHED_ALLOC_MUTEX_CACHE;
		new_sched->mutex_cache =
			_lthread_objcache_create("mutex cache",
						sizeof(struct lthread_mutex),
						LTHREAD_PREALLOC);
		if (new_sched->mutex_cache == NULL)
			break;

		alloc_status = SCHED_ALLOC_OK;
	} while (0);

	/* roll back on any failure */
	switch (alloc_status) {
	case SCHED_ALLOC_MUTEX_CACHE:
		_lthread_objcache_destroy(new_sched->cond_cache);
		/* fall through */
	case SCHED_ALLOC_COND_CACHE:
		_lthread_objcache_destroy(new_sched->tls_cache);
		/* fall through */
	case SCHED_ALLOC_TLS_CACHE:
		_lthread_objcache_destroy(new_sched->per_lthread_cache);
		/* fall through */
	case SCHED_ALLOC_PERLT_CACHE:
		_lthread_objcache_destroy(new_sched->stack_cache);
		/* fall through */
	case SCHED_ALLOC_STACK_CACHE:
		_lthread_objcache_destroy(new_sched->lthread_cache);
		/* fall through */
	case SCHED_ALLOC_LTHREAD_CACHE:
		_lthread_queue_destroy(new_sched->pready);
		/* fall through */
	case SCHED_ALLOC_PREADY_QUEUE:
		_lthread_queue_destroy(new_sched->ready);
		/* fall through */
	case SCHED_ALLOC_READY_QUEUE:
		_qnode_pool_destroy(new_sched->qnode_pool);
		/* fall through */
	case SCHED_ALLOC_QNODE_POOL:
		/* fall through */
	case SCHED_ALLOC_OK:
		break;
	}
	return alloc_status;
}

int init_Agent(int nb_cores){

	uint64_t core_mask = 0;
	int i = 0, index = 0;
	int Agent_id = 1001;
	printf(">>>init Agent %d\n", Agent_id);
	core_mask = registerAgent(Agent_id, 1, nb_cores);
	core_mask_count = core_mask;//high 56 bits for bitmap, low 8bits for count
	core_mask = core_mask>>8;
	printf(">>>apply core cnt=%d, mask=%d\n",core_mask_count&255, core_mask);
	while(core_mask>0){
		if((core_mask&1UL)==1){
			core_list[index++]=i;
			printf(">>>>get core %d\n", core_list[index-1]);
		}
		i++;
		core_mask = core_mask>>1;

	}
	rte_atomic16_set(&num_schedulers, (core_mask_count&255));
	return (core_mask_count&255);
}
/*
 * Create a scheduler on the current lcore
 */
struct lthread_sched *_lthread_sched_create(size_t stack_size)
{
	int status;
	struct lthread_sched *new_sched;
	unsigned lcoreid = rte_lcore_id();

	RTE_ASSERT(stack_size <= LTHREAD_MAX_STACK_SIZE);

	if (stack_size == 0)
		stack_size = LTHREAD_MAX_STACK_SIZE;

	new_sched =
	     rte_calloc_socket(NULL, 1, sizeof(struct lthread_sched),
				RTE_CACHE_LINE_SIZE,
				rte_socket_id());
//	printf("finish allocate sched\n");
	if (new_sched == NULL) {
		RTE_LOG(CRIT, LTHREAD,
			"Failed to allocate memory for scheduler\n");
		return NULL;
	}

	_lthread_key_pool_init();
	new_sched->stack_size = stack_size;
	new_sched->birth = rte_rdtsc();
	THIS_SCHED = new_sched;

	status = _lthread_sched_alloc_resources(new_sched);
	if (status != SCHED_ALLOC_OK) {
		RTE_LOG(CRIT, LTHREAD,
			"Failed to allocate resources for scheduler code = %d\n",
			status);
		rte_free(new_sched);
		return NULL;
	}

	bzero(&new_sched->ctx, sizeof(struct ctx));

	new_sched->lcore_id = lcoreid;

	schedcore[lcoreid] = new_sched;

	new_sched->run_flag = 1;

	DIAG_EVENT(new_sched, LT_DIAG_SCHED_CREATE, rte_lcore_id(), 0);

	rte_wmb();
	return new_sched;
}

/*
 * Set the number of schedulers in the system
 */

/*
 * Return the number of schedulers active
 */
int lthread_active_schedulers(void)
{
	return (int)rte_atomic16_read(&active_schedulers);
}


/**
 * shutdown the scheduler running on the specified lcore
 */
void lthread_scheduler_shutdown(unsigned lcoreid)
{
	uint64_t coreid = (uint64_t) lcoreid;

	if (coreid < LTHREAD_MAX_LCORES) {
		if (schedcore[coreid] != NULL)
			schedcore[coreid]->run_flag = 0;
	}
}

/**
 * shutdown all schedulers
 */
void lthread_scheduler_shutdown_all(void)
{
	uint64_t i;

	/*
	 * give time for all schedulers to have started
	 * Note we use sched_yield() rather than pthread_yield() to allow
	 * for the possibility of a pthread wrapper on lthread_yield(),
	 * something that is not possible unless the scheduler is running.
	 */
	while (rte_atomic16_read(&active_schedulers) <
	       rte_atomic16_read(&num_schedulers))
		sched_yield();

	for (i = 0; i < LTHREAD_MAX_LCORES; i++) {
		if (schedcore[i] != NULL)
			schedcore[i]->run_flag = 0;
	}
}

/*
 * Resume a suspended lthread
 */
static __rte_always_inline void
_lthread_resume(struct lthread *lt);
static inline void _lthread_resume(struct lthread *lt)
{
	struct lthread_sched *sched = THIS_SCHED;
	struct lthread_stack *s;
	uint64_t state = lt->state;
	int ret;
#if LTHREAD_DIAG
	int init = 0;
#endif


	sched->current_lthread = lt;

	if (state & (BIT(ST_LT_CANCELLED) | BIT(ST_LT_EXITED))) {
		printf("lt state = detach | cancle | exit\n");
		/* if detached we can free the thread now */
		if (state & BIT(ST_LT_DETACH)) {
			_lthread_free(lt);
			sched->current_lthread = NULL;
			return;
		}
	}

	if (state & BIT(ST_LT_INIT)) {
//		printf("lt state = init\n");
		/* first time this thread has been run */
		/* assign thread to this scheduler */
		lt->sched = THIS_SCHED;

		/* allocate stack */
		s = _stack_alloc();

		lt->stack_container = s;
		_lthread_set_stack(lt, s->stack, s->stack_size);

		/* allocate memory for TLS used by this thread */
		_lthread_tls_alloc(lt);

		lt->state = BIT(ST_LT_READY);
#if LTHREAD_DIAG
		init = 1;
#endif
	}

	DIAG_EVENT(lt, LT_DIAG_LTHREAD_RESUMED, init, lt);

	/* switch to the new thread */
//	if(lt->thread_id == 3)
//		printf("core %d switch to lt %d->ctx\n", sched->lcore_id, lt->thread_id);
	ctx_switch(&lt->ctx, &sched->ctx);
    //FIXME:thread call yield() return here

//    if(lt->thread_id == 3)
//        printf("lt %d return core %d\n", lt->thread_id, sched->lcore_id);

	/* If posting to a queue that could be read by another lcore
	 * we defer the queue write till now to ensure the context has been
	 * saved before the other core tries to resume it
	 * This applies to blocking on mutex, cond, and to set_affinity
	 */

//    printf("return core %d\n", sched->lcore_id);

	// possibility 1: 检查该线程是否被迁移到一个核，若是则lt->pending_wr_queue为迁移对象dst core的pready队列
	//possibility 2: 若线程等待mutex/con，则lt->pending_wr_queue为该mutex/con的blocked队列
	if (lt->pending_wr_queue != NULL) {
		struct lthread_queue *dest = lt->pending_wr_queue;

		lt->pending_wr_queue = NULL;

		/* queue the current thread to the specified queue */
		_lthread_queue_insert_mp(dest, lt);
	}

	sched->current_lthread = NULL;
}

/*
 * Handle sleep timer expiry
*/
void
_sched_timer_cb(struct rte_timer *tim, void *arg)
{
	struct lthread *lt = (struct lthread *) arg;
	uint64_t state = lt->state;

	DIAG_EVENT(lt, LT_DIAG_LTHREAD_TMR_EXPIRED, &lt->tim, 0);

	rte_timer_stop(tim);

	if (lt->state & BIT(ST_LT_CANCELLED))
		(THIS_SCHED)->nb_blocked_threads--;

	lt->state = state | BIT(ST_LT_EXPIRED);
	_lthread_resume(lt);
	lt->state = state & CLEARBIT(ST_LT_EXPIRED);
}



/*
 * Returns 0 if there is a pending job in scheduler or 1 if done and can exit.
 */
static inline int _lthread_sched_isdone(struct lthread_sched *sched)
{
	return (sched->run_flag == 0) &&
			(_lthread_queue_empty(sched->ready)) &&
			(_lthread_queue_empty(sched->pready)) &&
			(sched->nb_blocked_threads == 0);
}

/*
 * Wait for all schedulers to start
 */
static inline void _lthread_schedulers_sync_start(void)
{
	rte_atomic16_inc(&active_schedulers);

	/* wait for lthread schedulers
	 * Note we use sched_yield() rather than pthread_yield() to allow
	 * for the possibility of a pthread wrapper on lthread_yield(),
	 * something that is not possible unless the scheduler is running.
	 */
	while (rte_atomic16_read(&active_schedulers) <
	       rte_atomic16_read(&num_schedulers))
		sched_yield();

}

/*
 * Wait for all schedulers to stop
 */
static inline void _lthread_schedulers_sync_stop(void)
{
	rte_atomic16_dec(&active_schedulers);
	rte_atomic16_dec(&num_schedulers);

	/* wait for schedulers
	 * Note we use sched_yield() rather than pthread_yield() to allow
	 * for the possibility of a pthread wrapper on lthread_yield(),
	 * something that is not possible unless the scheduler is running.
	 */
	while (rte_atomic16_read(&active_schedulers) > 0)
		sched_yield();

}

/*
 * add for nfv
 * update drop rate vector of all nf threads
 */
uint8_t update_dr_vector(void){
	uint8_t ret = 0;

	return ret;
}



/*
 * Run the master thread scheduler
 */
void slave_scheduler_run(void){

	struct lthread_sched *sched = THIS_SCHED;
	struct lthread *lt = NULL;
    struct lthread *new_lt[127];

    int ret;
	int cnt = 0;
    int new_index = 0;

	RTE_LOG(INFO, LTHREAD,
			"starting master scheduler %p on lcore %u phys core %u\n",
			sched, rte_lcore_id(),
			rte_lcore_index(rte_lcore_id()));

	while (!_lthread_sched_isdone(sched)) {

		rte_timer_manage();
		update_dr_vector();
		cnt++;

		lt = _lthread_queue_poll(sched->ready);
		if (lt != NULL) {
//			if(cnt>100){
				//check drop rate
				ret = checkIsDrop(lt->thread_id);
				//	printf("thread %d call check drop, ret %d\n", lt->thread_id, ret);
				if(ret >=0){
					printf("thread %d check dv, get ret = %d\n",lt->thread_id, ret);
                    if(lt->belong_to_sfc == 0) {
                        lt->should_migrate = ret;
                    }
                    else{
                        //scale out sfc
                        printf("scale out sfc: %d->%d->%d\n", lt->thread_id, lt->next_hop_nf->thread_id, lt->next_hop_nf->next_hop_nf->thread_id);
                        //FIXME: now we must fix len of chain to 3
                        //TODO:how to scale out ring?
                        launch_sfc(&new_lt[new_index], &ret, lt->chain_len, lt->fun, lt->arg,
                        lt->next_hop_nf->fun, lt->next_hop_nf->arg,
                        lt->next_hop_nf->next_hop_nf->fun, lt->next_hop_nf->next_hop_nf->arg);
                        new_index += lt->chain_len;
                    }
				}else if(ret == -2){
					//TODO: call add core from CM
				}

			_lthread_resume(lt);
		}
		lt = _lthread_queue_poll(sched->pready);
		if (lt != NULL) {
			printf("core %d get a lt %d from pready queue\n", sched->lcore_id, lt->thread_id);
			_lthread_resume(lt);
		}
	}


	/* if more than one wait for all schedulers to stop */
	_lthread_schedulers_sync_stop();

	(THIS_SCHED) = NULL;

	RTE_LOG(INFO, LTHREAD,
			"stopping scheduler %p on lcore %u phys core %u\n",
			sched, rte_lcore_id(),
			rte_lcore_index(rte_lcore_id()));
	fflush(stdout);

}
/*
 * Run the slave thread scheduler
 * This loop is the heart of the system
// */
//void master_scheduler_run(void)
//{
//
//	struct lthread_sched *sched = THIS_SCHED;
//	struct lthread *lt = NULL;
//	uint16_t drop_rate = 0;
//
//	RTE_LOG(INFO, LTHREAD,
//		"starting slave scheduler %p on lcore %u phys core %u\n",
//		sched, rte_lcore_id(),
//		rte_lcore_index(rte_lcore_id()));
//
//	/* if more than one, wait for all schedulers to start */
//	_lthread_schedulers_sync_start();
//
//
//	/*
//	 * This is the main scheduling loop
//	 * So long as there are tasks in existence we run this loop.
//	 * We check for:-
//	 *   expired timers,
//	 *   the local ready queue,
//	 *   and the peer ready queue,
//	 *
//	 * and resume lthreads ad infinitum.
//	 */
//	while (!_lthread_sched_isdone(sched)) {
//
//		rte_timer_manage();
//
//		lt = _lthread_queue_poll(sched->ready);
//		if (lt != NULL) {
////			if(sched->lcore_id == 1 && lt->thread_id == 30)
////				printf("core %d get a lt %dfrom ready\n", sched->lcore_id, lt->thread_id);
//
//			// FIXME:for nfv test, modify later
////			printf("core %d check for drop rate\n", sched->lcore_id);
////			if(check_nf_droprate(sched->lcore_id, lt)>0){
////				lt->should_migrate = 1;
////				printf("scheduler mark lt %d to should migrate to 1\n", lt->thread_id);
////			}
////            if(lt->thread_id == 32 && sched->lcore_id == 1){
////                printf("core %d get lt %d from ready\n", sched->lcore_id, lt->thread_id);
////            }
////			if(sched->lcore_id!=0)
////				printf("core %d resum a lt %d\n", sched->lcore_id,lt->thread_id);
//			_lthread_resume(lt);
////			if(sched->lcore_id!=0)
////				printf("core %d finish lt %d\n", sched->lcore_id, lt->thread_id);
//
//		}
//		lt = _lthread_queue_poll(sched->pready);
//		if (lt != NULL) {
//			printf("core %d get a lt %d from pready queue\n", sched->lcore_id, lt->thread_id);
//			_lthread_resume(lt);
//		}
//	}
//
//
//	/* if more than one wait for all schedulers to stop */
//	_lthread_schedulers_sync_stop();
//
//	(THIS_SCHED) = NULL;
//
//	RTE_LOG(INFO, LTHREAD,
//		"stopping scheduler %p on lcore %u phys core %u\n",
//		sched, rte_lcore_id(),
//		rte_lcore_index(rte_lcore_id()));
//	fflush(stdout);
//}

/*
 * Return the scheduler for this lcore
 *
 */
struct lthread_sched *_lthread_sched_get(int lcore_id)
{
	if (lcore_id > LTHREAD_MAX_LCORES)
		return NULL;
	return schedcore[lcore_id];
}

/*
 * migrate the current thread to another scheduler running
 * on the specified lcore.
 */
int lthread_set_affinity(struct lthread *lt, unsigned lcoreid)
{
	if(lt == NULL)
		lt = THIS_LTHREAD;
//	struct lthread *lt = THIS_LTHREAD;
	struct lthread_sched *dest_sched;

	if (unlikely(lcoreid > LTHREAD_MAX_LCORES))
		return POSIX_ERRNO(EINVAL);


	DIAG_EVENT(lt, LT_DIAG_LTHREAD_AFFINITY, lcoreid, 0);

//	printf("callee set_affinity\n");
	dest_sched = schedcore[lcoreid];

	if (unlikely(dest_sched == NULL))
		return POSIX_ERRNO(EINVAL);

	if (likely(dest_sched != THIS_SCHED)) {
		lt->sched = dest_sched;
		printf("set lt %d to be scheduled on core %d\n", lt->thread_id, dest_sched->lcore_id);
		lt->pending_wr_queue = dest_sched->pready;
		_affinitize();
//		printf("finish _affinize\n");
		return 0;
	}
	return 0;
}