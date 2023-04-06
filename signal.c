/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "evsignal.h"
#include "log.h"

extern struct event_list signalqueue;

static short evsigcaught[NSIG];
static int needrecalc;
volatile sig_atomic_t evsignal_caught = 0;

// 全局的信号 event，一个内部的可读 event，每次有信号量触发时，触发一次这个 event 的 callback
// TODO: 注释的解释是：Wake up our notification mechanism， 不是很理解这里的作用，为啥还需要 sig_event来激活？用户注册的 event 不可以吗
static struct event ev_signal;
// 信号事件 socket pair
static int ev_signal_pair[2];
static int ev_signal_added;

/* Callback for when the signal handler write a byte to our signaling socket */
/**
 * @brief 收到信号后触发，因为 ev_signal 没有被设置为 persist，所以需要每次将 ev_signal 加入到 event_base 的监听中
 * 
 * @param fd 
 * @param what 
 * @param arg 
 */
static void evsignal_cb(int fd, short what, void *arg)
{
	static char signals[100];
	struct event *ev = arg;
	int n;

	n = read(fd, signals, sizeof(signals));
	if (n == -1)
		event_err(1, "%s: read", __func__);
	event_add(ev, NULL);
}

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/**
 * @brief 初始化 ev_signal 这个 event，监听它的读事件，读到信号后，触发 evsignal_cb
 * 
 * @param evsigmask 
 */
void
evsignal_init(sigset_t *evsigmask)
{
	sigemptyset(evsigmask);

	/* 
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.
	 */
	// 创建一对全双工通信的 socket，信号处理时会向一端写入，event loop 从另一端读取
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ev_signal_pair) == -1)
		event_err(1, "%s: socketpair", __func__);

	FD_CLOSEONEXEC(ev_signal_pair[0]);
	FD_CLOSEONEXEC(ev_signal_pair[1]);
	// 为 ev_signal 信号 event 设置可读事件
	event_set(&ev_signal, ev_signal_pair[1], EV_READ,
	    evsignal_cb, &ev_signal);
	// 设置为内部事件，不影响 event_base 的事件计数
	ev_signal.ev_flags |= EVLIST_INTERNAL;
}

/**
 * @brief 将 evsigmask 中的信号量设置为 ev 的 fd 值
 * 
 * @param evsigmask 
 * @param ev 
 * @return int 
 */
int
evsignal_add(sigset_t *evsigmask, struct event *ev)
{
	int evsignal;
	
	if (ev->ev_events & (EV_READ|EV_WRITE))
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
	evsignal = EVENT_SIGNAL(ev);
	sigaddset(evsigmask, evsignal);
	
	return (0);
}

/**
 * @brief 删除 event 的信号量
 * 
 * @param evsigmask 
 * @param ev 
 * @return int 
 */
int
evsignal_del(sigset_t *evsigmask, struct event *ev)
{
	int evsignal;

	evsignal = EVENT_SIGNAL(ev);
	sigdelset(evsigmask, evsignal);
	needrecalc = 1;

	return (sigaction(EVENT_SIGNAL(ev),(struct sigaction *)SIG_DFL, NULL));
}

/**
 * @brief 信号触发时，设置evsignal_caught为1，
 * 增加evsigcaught中该sig的触发计数，并向 ev_signal_pair[0] 写入一个字符，以触发 此文件中 ev_signal 的可读事件
 * 
 * @param sig 信号值，是 event 的 fd
 */
static void
evsignal_handler(int sig)
{
	evsigcaught[sig]++;
	evsignal_caught = 1;

	/* Wake up our notification mechanism */
	write(ev_signal_pair[0], "a", 1);
}

/**
 * @brief 遍历 信号event 队列， 用 sigaction 重新注册需要监听的信号量，以及收到信号后的响应事件
 * 
 * @param evsigmask 
 * @return int 
 */
int
evsignal_recalc(sigset_t *evsigmask)
{
	struct sigaction sa;
	struct event *ev;
	// 如果没添加，把信号事件添加到 event_base 的信号队列中
	if (!ev_signal_added) {
		ev_signal_added = 1;
		event_add(&ev_signal, NULL);
	}
	// 如果信号队列是空的而且不需要重算，直接返回
	if (TAILQ_FIRST(&signalqueue) == NULL && !needrecalc)
		return (0);
	needrecalc = 0;
	// 将 evsigmask 信号集中的信号添加到进程的 sigmask 中
	if (sigprocmask(SIG_BLOCK, evsigmask, NULL) == -1)
		return (-1);
	
	/* Reinstall our signal handler. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = evsignal_handler;
	sa.sa_mask = *evsigmask;
	sa.sa_flags |= SA_RESTART;
	// 遍历信号队列，注册新号事件
	TAILQ_FOREACH(ev, &signalqueue, ev_signal_next) {
		if (sigaction(EVENT_SIGNAL(ev), &sa, NULL) == -1)
			return (-1);
	}
	return (0);
}

/**
 * @brief 将 evsigmask 信号集中的信号解除屏蔽
 * 
 * @param evsigmask 
 * @return int 
 */
int
evsignal_deliver(sigset_t *evsigmask)
{
	if (TAILQ_FIRST(&signalqueue) == NULL)
		return (0);

	return (sigprocmask(SIG_UNBLOCK, evsigmask, NULL));
	/* XXX - pending signals handled here */
}

/**
 * @brief 处理信号 event，通过 evsigcaught[event->fd] 判断触发次数，将信号 event 放入 event_base 的激活队列
 * 
 */
void
evsignal_process(void)
{
	struct event *ev;
	short ncalls;
	// 遍历信号 event 队列，找出有收到信号的 event，将其加入激活队列
	TAILQ_FOREACH(ev, &signalqueue, ev_signal_next) {
		ncalls = evsigcaught[EVENT_SIGNAL(ev)];
		if (ncalls) {
			if (!(ev->ev_events & EV_PERSIST))
				event_del(ev);
			event_active(ev, EV_SIGNAL, ncalls);
		}
	}

	memset(evsigcaught, 0, sizeof(evsigcaught));
	evsignal_caught = 0;
}

