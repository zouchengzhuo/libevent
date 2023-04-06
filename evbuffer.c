/*
 * Copyright (c) 2002-2004 Niels Provos <provos@citi.umich.edu>
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

#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#include "event.h"

/* prototypes */

void bufferevent_setwatermark(struct bufferevent *, short, size_t, size_t);
void bufferevent_read_pressure_cb(struct evbuffer *, size_t, size_t, void *);

/**
 * @brief 将一个 event 添加到 event_base 中
 * 
 * @param ev event
 * @param timeout 超时时间，单位秒
 * @return int  
 */
static int
bufferevent_add(struct event *ev, int timeout)
{
	struct timeval tv, *ptv = NULL;

	if (timeout) {
		timerclear(&tv);
		tv.tv_sec = timeout;
		ptv = &tv;
	}

	return (event_add(ev, ptv));
}

/* 
 * This callback is executed when the size of the input buffer changes.
 * We use it to apply back pressure on the reading side.
 */

/**
 * @brief 当 bufferevent 的输入缓冲区 input 这个 evbuffer 中的数据被消费，导致实际数据长度在高水位线之下之后
 * 将 bufferevent 的读取事件重新注册会 event_base 的监听，以继续从fd中读取数据
 * 
 * @param buf 
 * @param old 
 * @param now 
 * @param arg 
 */
void
bufferevent_read_pressure_cb(struct evbuffer *buf, size_t old, size_t now,
    void *arg) {
	struct bufferevent *bufev = arg;
	/* 
	 * If we are below the watermak then reschedule reading if it's
	 * still enabled.
	 */
	// 如果 evbuffer 所属的 bufferevent 没有高水位线，或者有高水位线但是当前数据在高水位线之下
	if (bufev->wm_read.high == 0 || now < bufev->wm_read.high) {
		// evbuffer 触发一次回调之后，将回调移除
		evbuffer_setcb(buf, NULL, NULL);
		// 如果 bufferevent 启用了读事件，将其再次加入到 event_base 的监听中
		if (bufev->enabled & EV_READ)
			bufferevent_add(&bufev->ev_read, bufev->timeout_read);
	}
}

/**
 * @brief eventbuffer 绑定的 fd 的可读回调，尝试从 fd 中读取数据放到 input 缓冲区中，并控制水位线
 * 
 * @param fd 目标 fd
 * @param event 可读 event
 * @param arg eventbuffer 对象指针
 */
static void
bufferevent_readcb(int fd, short event, void *arg)
{
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_READ;
	size_t len;
	// 如果是读超时，直接跳到错误处理逻辑
	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto error;
	}
	// 尝试读4k字节的数据到 eventbuffer 的读取缓冲区中
	res = evbuffer_read(bufev->input, fd, -1);
	if (res == -1) {
		// 如果是非阻塞 fd 报无数据，进入重新调度逻辑，把fd重新加入到 event_base*中
		if (errno == EAGAIN || errno == EINTR)
			goto reschedule;
		/* error case */
		what |= EVBUFFER_ERROR;
	} else if (res == 0) {
		/* eof case */
		// 读到数据长度为0，标识 end of file
		what |= EVBUFFER_EOF;
	}
	// res小于0且不是非阻塞fd，则进入错误处理逻辑
	if (res <= 0)
		goto error;
	// 成功读取了一次数据，将可读事件再次加入 event_base 的监听
	bufferevent_add(&bufev->ev_read, bufev->timeout_read);

	/* See if this callbacks meets the water marks */
	// 读取缓冲区中当前的实际数据长度
	len = EVBUFFER_LENGTH(bufev->input);
	// 如果低水位线的长度不为0，而且当前实际数据长度不到低水位线，则啥也不做
	if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)
		return;
	// 如果高水位线的值不为0，而且当前实际数据长度大于高水位线
	// 说明已经读过头了，不再读了，把 bufferevent 的可读事件从 event_base 的监听中移除
	if (bufev->wm_read.high != 0 && len > bufev->wm_read.high) {
		struct evbuffer *buf = bufev->input;

		event_del(&bufev->ev_read);

		/* Now schedule a callback for us */
		// 为 input buffer 设置一个回调函数
		evbuffer_setcb(buf, bufferevent_read_pressure_cb, bufev);
		return;
	}
	// 如果读取缓冲区的实际数据长度在高低水位线之间，则调用一次用户的读取回调
	/* Invoke the user callback - must always be called last */
	(*bufev->readcb)(bufev, bufev->cbarg);
	return;

 reschedule:
	// 将 read事件重新加入 event_base 的监听中
	bufferevent_add(&bufev->ev_read, bufev->timeout_read);
	return;

 error:
	// 调用用户设置的错误回调
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

/**
 * @brief 
 * 
 * @param fd 
 * @param event 
 * @param arg 
 */
static void
bufferevent_writecb(int fd, short event, void *arg)
{
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_WRITE;
	// 如果是可写超时回调，直接处理异常
	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto error;
	}
	// 如果 output 写缓冲区内实际数据长度大于0，则尝试把数据写入fd
	if (EVBUFFER_LENGTH(bufev->output)) {
		// 尝试把输出缓冲区中的数据数据全部写入fd
	    res = evbuffer_write(bufev->output, fd);
	    if (res == -1) {
			// 非阻塞 fd 报不可写，重新调度
		    if (errno == EAGAIN ||
			errno == EINTR ||
			errno == EINPROGRESS)
			    goto reschedule;
		    /* error case */
			// 其它问题，报错
		    what |= EVBUFFER_ERROR;
	    } else if (res == 0) {
		    /* eof case */
			//收到了eof，把eof标记加入到 what中
		    what |= EVBUFFER_EOF;
	    }
	    if (res <= 0)
		    goto error;
	}
	// 如果还没写完，把可写事件重新加回 event_base 的监听，等待下一轮
	if (EVBUFFER_LENGTH(bufev->output) != 0)
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	/*
	 * Invoke the user callback if our buffer is drained or below the
	 * low watermark.
	 */
	// 输出缓冲区中的数据写到低水位线之下后，调用一次用户的写数据回调
	if (EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)
		(*bufev->writecb)(bufev, bufev->cbarg);

	return;

 reschedule:
	// 若输出缓冲区中仍有数据，重新调度
	if (EVBUFFER_LENGTH(bufev->output) != 0)
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);
	return;

 error:
	// 处理错误
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

/*
 * Create a new buffered event object.
 *
 * The read callback is invoked whenever we read new data.
 * The write callback is invoked whenever the output buffer is drained.
 * The error callback is invoked on a write/read error or on EOF.
 */

/**
 * @brief 初始化一个 bufferevent 对象，设置好 读写缓冲区、读写事件、读写回调、出错回调
 * 
 * @param fd 目标fd
 * @param readcb 读取回调
 * @param writecb 写入回调
 * @param errorcb 出错回调
 * @param cbarg 回调时携带的参数
 * @return struct bufferevent* 
 */
struct bufferevent *
bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb,
    everrorcb errorcb, void *cbarg)
{
	struct bufferevent *bufev;
	// 初始化 bufferevent 并分配空间
	if ((bufev = calloc(1, sizeof(struct bufferevent))) == NULL)
		return (NULL);
	// 创建输入缓冲区 evbuffer
	if ((bufev->input = evbuffer_new()) == NULL) {
		free(bufev);
		return (NULL);
	}
	// 创建输出缓冲区 evbuffer
	if ((bufev->output = evbuffer_new()) == NULL) {
		evbuffer_free(bufev->input);
		free(bufev);
		return (NULL);
	}
	// 绑定 fd 的读事件到 bufferevent 的 ev_read
	event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);
	// 绑定 fd 的写事件到 bufferevent 的 ev_write
	event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);
	// 各类回调函数赋值
	bufev->readcb = readcb;
	bufev->writecb = writecb;
	bufev->errorcb = errorcb;
	// 回调参数赋值
	bufev->cbarg = cbarg;
	// enabled 固定为 读写都开启
	bufev->enabled = EV_READ | EV_WRITE;

	return (bufev);
}

/**
 * @brief 给 bufferevent 的可读可写事件设置优先级
 * 
 * @param bufev 
 * @param priority 
 * @return int 
 */
int
bufferevent_priority_set(struct bufferevent *bufev, int priority)
{
	if (event_priority_set(&bufev->ev_read, priority) == -1)
		return (-1);
	if (event_priority_set(&bufev->ev_write, priority) == -1)
		return (-1);

	return (0);
}

/* Closing the file descriptor is the responsibility of the caller */
/**
 * @brief 释放 bufferevent
 * 
 * @param bufev 
 */
void
bufferevent_free(struct bufferevent *bufev)
{
	event_del(&bufev->ev_read);
	event_del(&bufev->ev_write);

	evbuffer_free(bufev->input);
	evbuffer_free(bufev->output);

	free(bufev);
}

/*
 * Returns 0 on success;
 *        -1 on failure.
 */

/**
 * @brief 想 bufferevent 的输出缓冲区中写入数据，写完之后把 bufferevent 的可写事件加入到 event_base 的监听中
 * 
 * @param bufev 
 * @param data 
 * @param size 
 * @return int 
 */
int
bufferevent_write(struct bufferevent *bufev, void *data, size_t size)
{
	int res;

	res = evbuffer_add(bufev->output, data, size);

	if (res == -1)
		return (res);

	/* If everything is okay, we need to schedule a write */
	if (size > 0 && (bufev->enabled & EV_WRITE))
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	return (res);
}

/**
 * @brief 同 bufferevent_write，只是输入对象换成了一个 evbuffer
 * 
 * @param bufev 
 * @param buf 
 * @return int 
 */
int
bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf)
{
	int res;

	res = bufferevent_write(bufev, buf->buffer, buf->off);
	if (res != -1)
		evbuffer_drain(buf, buf->off);

	return (res);
}

/**
 * @brief 尝试从 bufferevent 的输入缓冲区中读取 size 数据，数据读到 data 上，返回读取成功的尺寸
 * 
 * @param bufev 
 * @param data 
 * @param size 
 * @return size_t 
 */
size_t
bufferevent_read(struct bufferevent *bufev, void *data, size_t size)
{
	struct evbuffer *buf = bufev->input;

	if (buf->off < size)
		size = buf->off;

	/* Copy the available data to the user buffer */
	memcpy(data, buf->buffer, size);

	if (size)
		evbuffer_drain(buf, size);

	return (size);
}

/**
 * @brief 启用 bufferevent 的 可读/可写事件
 * 
 * @param bufev 
 * @param event 
 * @return int 
 */
int
bufferevent_enable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (bufferevent_add(&bufev->ev_read, bufev->timeout_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (bufferevent_add(&bufev->ev_write, bufev->timeout_write) == -1)
			return (-1);
	}

	bufev->enabled |= event;
	return (0);
}

int
bufferevent_disable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (event_del(&bufev->ev_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (event_del(&bufev->ev_write) == -1)
			return (-1);
	}

	bufev->enabled &= ~event;
	return (0);
}

/*
 * Sets the read and write timeout for a buffered event.
 */

void
bufferevent_settimeout(struct bufferevent *bufev,
    int timeout_read, int timeout_write) {
	bufev->timeout_read = timeout_read;
	bufev->timeout_write = timeout_write;
}

/*
 * Sets the water marks
 */

void
bufferevent_setwatermark(struct bufferevent *bufev, short events,
    size_t lowmark, size_t highmark)
{
	if (events & EV_READ) {
		bufev->wm_read.low = lowmark;
		bufev->wm_read.high = highmark;
	}

	if (events & EV_WRITE) {
		bufev->wm_write.low = lowmark;
		bufev->wm_write.high = highmark;
	}

	/* If the watermarks changed then see if we should call read again */
	bufferevent_read_pressure_cb(bufev->input,
	    0, EVBUFFER_LENGTH(bufev->input), bufev);
}
