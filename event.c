/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include "misc.h"
#endif
#include <sys/types.h>
#include <sys/tree.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "log.h"

#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_RTSIG
extern const struct eventop rtsigops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
const struct eventop *eventops[] = {
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_RTSIG
	&rtsigops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_list signalqueue;

struct event_base *current_base = NULL;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);	/* Signal callback when gotsig is set */
volatile int event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval *);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

static int
compare(struct event *a, struct event *b)
{
	if (timercmp(&a->ev_timeout, &b->ev_timeout, <))
		return (-1);
	else if (timercmp(&a->ev_timeout, &b->ev_timeout, >))
		return (1);
	if (a < b)
		return (-1);
	else if (a > b)
		return (1);
	return (0);
}

RB_PROTOTYPE(event_tree, event, ev_timeout_node, compare);

RB_GENERATE(event_tree, event, ev_timeout_node, compare);


void *
event_init(void)
{
	int i;
	// 分配一个 event_base 的空间，赋值给 current_base
	if ((current_base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc");

	event_sigcb = NULL;
	event_gotsig = 0;
	// 设置 event_base 的当前时间
	gettimeofday(&current_base->event_tv, NULL);
	// 初始化用于存放 event 对象的红黑树
	RB_INIT(&current_base->timetree);
	// 初始化用于存放 event 的链表
	TAILQ_INIT(&current_base->eventqueue);
	// 初始化用于存放 signal 的链表
	TAILQ_INIT(&signalqueue);
	
	// 根据不同平台选择一个多路复用系统，选择后进行初始化
	current_base->evbase = NULL;
	for (i = 0; eventops[i] && !current_base->evbase; i++) {
		current_base->evsel = eventops[i];

		current_base->evbase = current_base->evsel->init();
	}
	// 如果没找到任何可用的多路复用组件，则报错
	if (current_base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);
	// 输出使用的多路复用库信息
	if (getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   current_base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(current_base, 1);

	return (current_base);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

/**
 * @brief 设置 event_base 优先级数量
 * 
 * @param base event_base 对象指针
 * @param npriorities 优先级数量
 * @return int 
 */
int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;
	// 如果已经有了激活状态的 event 了，就不能设置了
	if (base->event_count_active)
		return (-1);
	// 如果之前设置过且和现在不一样，则清空之前的
	if (base->nactivequeues && npriorities != base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	// 分配 npriorities 个激活队列的空间
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)calloc(base->nactivequeues,
	    npriorities * sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);
	// 分配优先级队列列表中，每个队列的空间
	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

/**
 * @brief 处理激活队列中的事件，每个事件都根据 ev_ncalls 调用对应次数的回调函数，并将其从激活队列中移除
 * 
 * @param base 
 */
static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;
	// 没有激活事件，直接返回
	if (!base->event_count_active)
		return;
	// 取出第一个激活状态的事件队列（一次只处理一个优先级）
	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		// 每处理一个，就从激活队列中移除一个
		event_queue_remove(base, ev, EVLIST_ACTIVE);
		
		/* Allows deletes to work */
		// 读取 event 触发事件的数量
		ncalls = ev->ev_ncalls;
		// 将 ev_pncalls 设置为上一轮激活事件的数量
		ev->ev_pncalls = &ncalls;
		// 根据激活次数，调用n次回调函数
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			// 调用回调时传入参数： fd、激活的事件类型、ev中附带的args
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */

int
event_loopexit(struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)
{
	// 取出 event_base 信息
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	int res, done;

	/* Calculate the initial events that we are waiting for */
	// 如果没有监听的 event，直接退出
	if (evsel->recalc(base, evbase, 0) == -1)
		return (-1);

	done = 0;
	while (!done) {
		/* Terminate the loop if we have been asked to */
		// 收到停止信号时，退出事件循环
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		// TODO: 信号处理逻辑，就只是在每个事件循环里边判断一下？纵观代码，这个应该是用不上的
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}

		/* Check if time is running backwards */
		// 检查时间是否在倒退，如果倒退，对超时事件红黑树中的所有 events 的超时时间进行修正，减去要给 offset
		gettimeofday(&tv, NULL);
		if (timercmp(&tv, &base->event_tv, <)) {
			struct timeval off;
			event_debug(("%s: time is running backwards, corrected",
				    __func__));
			timersub(&base->event_tv, &tv, &off);
			timeout_correct(base, &off);
		}
		// 更新 event_base 的 event_tv 时间
		base->event_tv = tv;
		// 计算下一次 epoll_wait 的超时时间
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK))
			// 如果一个激活状态的 event 都没有，而且 flag 中不包含 EVLOOP_NONBLOCK，即是一个阻塞epoll（从 event_base_dispatch 调用进来的 flag 是默认值0）
			// 那么计算得到一个 epoll wait 超时时间 tv
			timeout_next(base, &tv);
		else
			// 否则将tv设置为时间 0 
			timerclear(&tv);
		
		/* If we have no events, we just exit */
		// 如果没有被监听的 fd，直接退出
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}
		// 执行多路复用的 wait 操作，使用上面计算得到的 tv，如果 tv 是0，则1微秒超时
		// epoll_wait 超时后，若有读写事件发生，将有事件发生的 event 放到激活队列中
		res = evsel->dispatch(base, evbase, &tv);
		// 如果出错，直接返回
		if (res == -1)
			return (-1);
		// 处理超时事件，若有超时的事件，给放到激活队列中，然后从超时事件红黑树和其它结构中移除，也就是说超时事件仅触发一次
		timeout_process(base);
		// 如果存在激活状态的 event，处理激活事件
		if (base->event_count_active) {
			// 处理一个优先级最高的激活事件队列，处理完毕后从激活队列中移除，其它优先级的等待下一次处理
			event_process_active(base);
			// 如果事件处理完了，而且 flat 中有 EVLOOP_ONCE，则退出
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			// 如果没有激活状态的 event，而且 event_base 设置了 EVLOOP_NONBLOCK flag，则直接退出
			done = 1;

		if (evsel->recalc(base, evbase, 0) == -1)
			return (-1);
	}

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* Schedules an event once */

int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	event_add(&eonce->ev, tv);

	return (0);
}

void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	// 设置当前的 event_base 为默认的 current_base
	ev->ev_base = current_base;
	// 设置事件触发后的回调
	ev->ev_callback = callback;
	// 设置事件触发后传给回调的参数
	ev->ev_arg = arg;
	// 要加入监听的 fd，如果是信号事件，则传入要监听的信号值
	ev->ev_fd = fd;
	// 要监听的事件列表
	ev->ev_events = events;
	// 设置状态 flag 为初始
	ev->ev_flags = EVLIST_INIT;
	// 事件被触发的次数
	ev->ev_ncalls = 0;
	// 事件上次被激活后触发的次数
	// 如果该事件在回调函数中被重新注册，那么ev_pncalls会被设置为ev_ncalls的值，表示该事件在下一次被激活时应该触发ev_ncalls-ev_pncalls次。
	// 这样可以确保事件在回调函数中被重新注册后，不会立即被再次触发，从而避免事件处理器进入死循环。
	ev->ev_pncalls = NULL;

	/* by default, we put new events into the middle priority */
	// 设置默认优先级，为 event_base 的 活跃队列数量的 一半，即默认中等优先级
	ev->ev_pri = current_base->nactivequeues/2;
}

int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */
/**
 * 设置一个 event 的优先级，如果这个 event 已经是激活状态的了，会设置失败，因为它已经进了一个队列了
*/
int
event_priority_set(struct event *ev, int pri)
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;
	if (ev->ev_flags & EVLIST_SIGNAL)
		flags |= EV_SIGNAL;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT))
		*tv = ev->ev_timeout;

	return (flags & event);
}

int
event_add(struct event *ev, struct timeval *tv)
{
	// 从 event 中取出 event_base 信息
	struct event_base *base = ev->ev_base;
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));
	// 如果绑定时附带了超时信息
	if (tv != NULL) {
		struct timeval now;
		// 判断 event 的 flag，如果已经在超时队列中了，则给移除掉
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		// 如果 event 已经在 激活队列中了，而且是因为超时而激活的，则重新添加时，在超时回调执行之前将其从激活队列中移除
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 */
			// 清空触发次数，之前触发的就不会调用回调了
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			// 从激活队列中移除
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		// 将 event 的 ev_timeout 设置为 当前时间点加上传入的 tv timeval
		gettimeofday(&now, NULL);
		timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
			 "event_add: timeout in %d seconds, call %p",
			 tv->tv_sec, ev->ev_callback));
		// 将 event 插入超时队列，timeout事件为了快速查找写入红黑树，给 event 的 flag 添加 EVLIST_TIMEOUT 的flag
		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}
	// 如果 event 监听了 可读或者可写事件，而且它不在 event_base 的已插入队列或者活跃队列中，则将它加入已插入队列
	if ((ev->ev_events & (EV_READ|EV_WRITE)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		event_queue_insert(base, ev, EVLIST_INSERTED);
		// 执行多路复用管理器的add操作，比如 epoll_ctl add
		return (evsel->add(evbase, ev));
	} else if ((ev->ev_events & EV_SIGNAL) &&
	    !(ev->ev_flags & EVLIST_SIGNAL)) {
		// 如果 event 监听了 signal 事件，而且不在 event_base 的 signal 队列中，则加入
		event_queue_insert(base, ev, EVLIST_SIGNAL);
		// 执行多路复用管理器的add操作，比如 epoll_ctl add
		return (evsel->add(evbase, ev));
	}

	return (0);
}

int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	} else if (ev->ev_flags & EVLIST_SIGNAL) {
		event_queue_remove(base, ev, EVLIST_SIGNAL);
		return (evsel->del(evbase, ev));
	}

	return (0);
}

/**
 * @brief 将 event 激活并放入到激活队列中
 * 
 * @param ev event
 * @param res 触发事件，如 EV_TIMEOUT / EV_READ / EV_WRITE
 * @param ncalls 触发次数
 */
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	// 如果event已经是活跃，将 res 事件合并到 ev_res中
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}
	// 写入本次激活触发的事件，如 EV_TIMEOUT / EV_READ / EV_WRITE
	ev->ev_res = res;
	// 写入激活次数
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	// 将 event 插入到激活队列中
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

/**
 * @brief 计算 epoll_wait 的超时时间，若没有超时 event，返回默认的 5s；若有超时的 event，则返回当前时间距离最近超时 event 的时间差（最小值为0）。
 * 
 * @param base event_base 对象
 * @param tv epoll_wait 的超时时间
 * @return int 
 */
int
timeout_next(struct event_base *base, struct timeval *tv)
{
	// 默认超时时间，5秒
	struct timeval dflt = TIMEOUT_DEFAULT;

	struct timeval now;
	struct event *ev;
	// 从超时红黑树中取出超时时间最小的 event，如果没有，则将 tv 设置为默认超时时间，并返回
	if ((ev = RB_MIN(event_tree, &base->timetree)) == NULL) {
		*tv = dflt;
		return (0);
	}
	// 获取当前时间戳
	if (gettimeofday(&now, NULL) == -1)
		return (-1);
	// 如果 event 的超时时间小于当前时间，即 event 已经超时了，将 tv 设置为 0，返回
	if (timercmp(&ev->ev_timeout, &now, <=)) {
		timerclear(tv);
		return (0);
	}
	// event 还没超时，用 event 的超时时间点减去当前时间点，作为 tv 的值，返回
	timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %d seconds", tv->tv_sec));
	return (0);
}

/**
 * @brief 发现时间倒退时，修正超时 event 红黑树中的超时时间
 * 
 * @param base 
 * @param off 
 */
static void
timeout_correct(struct event_base *base, struct timeval *off)
{
	// 当时间出现倒退时，修正超时红黑树中的时间差
	struct event *ev;

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	RB_FOREACH(ev, event_tree, &base->timetree)
		timersub(&ev->ev_timeout, off, &ev->ev_timeout);
}

/**
 * @brief 处理超时队列中的 event，判断是否超时，已经超时的从其它队列移除，给放到激活队列中
 * 
 * @param base 
 */
void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev, *next;
	// 获取当前时间
	gettimeofday(&now, NULL);
	// 从红黑树中查找最小节点，并依次查找下一个节点
	for (ev = RB_MIN(event_tree, &base->timetree); ev; ev = next) {
		// 如果超时时间还没到，直接结束此函数
		if (timercmp(&ev->ev_timeout, &now, >))
			break;
		// 已经超时，查找下一个 event
		next = RB_NEXT(event_tree, &base->timetree, ev);
		// 将 event 从 timeout queue中移除
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* delete this event from the I/O queues */
		// 把此 event 从所有的事件队列中移除
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		// 触发一次 EV_TIMEOUT 事件
		event_active(ev, EV_TIMEOUT, 1);
	}
}

/**
 * @brief 根据 queue 的类型，从各种容器中移除队列
 * 
 * @param base event_base 指针
 * @param ev event 事件指针
 * @param queue 队列类型，可能是超时队列、激活队列、信号队列等
 */
void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	int docount = 1;

	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	// 如果是内部事件，不减少event计数
	if (ev->ev_flags & EVLIST_INTERNAL)
		docount = 0;
	// event_base 绑定的事件数量-1
	if (docount)
		base->event_count--;

	ev->ev_flags &= ~queue;
	// 根据不同的状态，从不同的容器中移除 event
	switch (queue) {
	case EVLIST_ACTIVE:
		// 激活事件数量-1
		if (docount)
			base->event_count_active--;
		// event 从激活队列中移除
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_SIGNAL:
		// 从信号队列中移除
		TAILQ_REMOVE(&signalqueue, ev, ev_signal_next);
		break;
	case EVLIST_TIMEOUT:
		// 从超时事件的红黑树中移除
		RB_REMOVE(event_tree, &base->timetree, ev);
		break;
	case EVLIST_INSERTED:
		// 从全量事件队列中移除
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/**
 * @brief 根据 queue 类型将 event 插入到 event_base 的队列
 * 
 * @param base 
 * @param ev 
 * @param queue 
 */
void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	int docount = 1;
	// 如果已经在目标队列中了，直接返回
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}
	// 如果是内部事件，则不增加事件总数
	if (ev->ev_flags & EVLIST_INTERNAL)
		docount = 0;
	// 增加事件总数
	if (docount)
		base->event_count++;
	// 将 queue 加入到 event 的 flag 中
	ev->ev_flags |= queue;
	switch (queue) {
	case EVLIST_ACTIVE:
		// 加入到激活队列并增加激活队列长度标记
		if (docount)
			base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_SIGNAL:
		// 加入到信号队列
		TAILQ_INSERT_TAIL(&signalqueue, ev, ev_signal_next);
		break;
	case EVLIST_TIMEOUT: {
		// 加入到超时事件红黑树
		struct event *tmp = RB_INSERT(event_tree, &base->timetree, ev);
		assert(tmp == NULL);
		break;
	}
	case EVLIST_INSERTED:
		// 加入到总队列
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
