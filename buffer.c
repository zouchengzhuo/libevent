/*
 * Copyright (c) 2002, 2003 Niels Provos <provos@citi.umich.edu>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_VASPRINTF
/* If we have vasprintf, we need to define this before we include stdio.h. */
#define _GNU_SOURCE
#endif

#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "event.h"

/**
 * @brief 创建一个 evbuffer 对象并返回其指针
 * 
 * @return struct evbuffer* 
 */
struct evbuffer *
evbuffer_new(void)
{
	struct evbuffer *buffer;
	
	buffer = calloc(1, sizeof(struct evbuffer));

	return (buffer);
}

/**
 * @brief 释放一个 evbuffer 对象的空间
 * 
 * @param buffer 
 */
void
evbuffer_free(struct evbuffer *buffer)
{
	if (buffer->orig_buffer != NULL)
		free(buffer->orig_buffer);
	free(buffer);
}

/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */
// 很奇怪，取名为 SWAP，但是实际上是单方面赋值
#define SWAP(x,y) do { \
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

/**
 * @brief 将 inbuf 的实际内容剪切到 outbuf 中
 * 
 * @param outbuf 目标 buf
 * @param inbuf 数据 buf
 * @return int 
 */
int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	int res;

	/* Short cut for better performance */
	// 如果outbuf的实际数据大小为0，则做特殊处理，直接交换两者的内容
	if (outbuf->off == 0) {
		struct evbuffer tmp;
		// 记录下 inbuf 原本的实际数据长度
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		// 直接把 inbuf 和 outbuf 的内容交换
		SWAP(&tmp, outbuf);
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		/* 
		 * Optimization comes with a price; we need to notify the
		 * buffer if necessary of the changes. oldoff is the amount
		 * of data that we tranfered from inbuf to outbuf
		 */
		// 分别调用 inbuf、outbuf 的回调
		if (inbuf->off != oldoff && inbuf->cb != NULL)
			(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		if (oldoff && outbuf->cb != NULL)
			(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}
	// 将 inbuf 的实际数据添加到 outbuf 中
	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);
	// 如果添加成功，清空 inbuf 的实际数据
	if (res == 0)
		evbuffer_drain(inbuf, inbuf->off);

	return (res);
}

/**
 * @brief 将格式化的数据添加到 evbuffer 中，如：evbuffer_add_printf(buf, "hello, %s", "world");
 * 
 * @param buf 目标 evbufer
 * @param fmt format字符串
 * @param ... 参数列表
 * @return int 
 */
int
evbuffer_add_printf(struct evbuffer *buf, char *fmt, ...)
{
	int res = -1;
	char *msg;
#ifndef HAVE_VASPRINTF
	static char buffer[4096];
#endif
	va_list ap;

	va_start(ap, fmt);

#ifdef HAVE_VASPRINTF
	if (vasprintf(&msg, fmt, ap) == -1)
		goto end;
#else
#  ifdef WIN32
	_vsnprintf(buffer, sizeof(buffer) - 1, fmt, ap);
	buffer[sizeof(buffer)-1] = '\0';
#  else /* ! WIN32 */
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
#  endif
	msg = buffer;
#endif
	
	res = strlen(msg);
	if (evbuffer_add(buf, msg, res) == -1)
		res = -1;
#ifdef HAVE_VASPRINTF
	free(msg);

end:
#endif
	va_end(ap);

	return (res);
}

/* Reads data from an event buffer and drains the bytes read */
/**
 * @brief 读取并删除一段数据，读取数据长度大于实际长度时，只读出实际长度
 * 
 * @param buf 被读取并清理的 evbuffer
 * @param data 读取并写入数据的目标地址
 * @param datlen 读取并清理的数据长度
 * @return int 实际读取并清理的数据长度
 */
int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	size_t nread = datlen;
	// 不能读取超过实际长度
	if (nread >= buf->off)
		nread = buf->off;
	// 实际数据拷贝到 data 内存块中
	memcpy(data, buf->buffer, nread);
	// evbuffer 中清理掉被读取的长度
	evbuffer_drain(buf, nread);
	// 返回实际读取长度
	return (nread);
}

/*
 * Reads a line terminated by either '\r\n', '\n\r' or '\r' or '\n'.
 * The returned buffer needs to be freed by the called.
 */
/**
 * @brief 从 evbuffer 中读取一行以 '\r\n', '\n\r' or '\r' or '\n' 结尾的数据
 * 
 * @param buffer 被读取的 evbuffer
 * @return char* 读取到的行，注意，此块数据需要由调用者释放
 */
char *
evbuffer_readline(struct evbuffer *buffer)
{
	// 实际数据起点地址
	u_char *data = EVBUFFER_DATA(buffer);
	// 实际数据长度
	size_t len = EVBUFFER_LENGTH(buffer);
	// 被读取的行
	char *line;
	u_int i;
	// 逐个字符判断是否是 \r 或者 \n
	for (i = 0; i < len; i++) {
		if (data[i] == '\r' || data[i] == '\n')
			break;
	}
	// 没有，则返回null
	if (i == len)
		return (NULL);
	// 为 line 分配内存块
	if ((line = malloc(i + 1)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", __func__);
		evbuffer_drain(buffer, i);
		return (NULL);
	}
	// 拷贝行数据到 line 中
	memcpy(line, data, i);
	line[i] = '\0';

	/*
	 * Some protocols terminate a line with '\r\n', so check for
	 * that, too.
	 */
	// 检查 \r\n 或者 \n\r 的情况，如果是，清理的时候多清理一段
	if ( i < len - 1 ) {
		char fch = data[i], sch = data[i+1];

		/* Drain one more character if needed */
		if ( (sch == '\r' || sch == '\n') && sch != fch )
			i += 1;
	}
	// 把已经读取的行清理掉
	evbuffer_drain(buffer, i + 1);

	return (line);
}

/* Adds data to an event buffer */
/**
 * @brief 对 evbuffer 中的数据进行一次对齐
 * 
 * @param buf 目标 evbuffer
 */
static inline void
evbuffer_align(struct evbuffer *buf)
{
	// 将 evbuffer 的实际数据块，移动到内存块的起点
	memmove(buf->orig_buffer, buf->buffer, buf->off);
	// buffer 指针修改为 orig_buffer 的相同位置
	buf->buffer = buf->orig_buffer;
	// 实际数据错位 offset 修改为0
	buf->misalign = 0;
}

/* Expands the available space in the event buffer to at least datlen */
/**
 * @brief 对 buf 的内存区域进行扩容，最小256字节，每次翻倍扩容。扩容前先对数据进行强制对齐。
 * 
 * @param buf 要扩容的 evbuffer
 * @param datlen 本次扩容后要插入的数据长度
 * @return int 
 */
int
evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
	// 当前需要的内存块长度
	size_t need = buf->misalign + buf->off + datlen;

	// 如果已经满足要求，就啥也不用做了
	if (buf->totallen >= need)
		return (0);

	/*
	 * If the misalignment fulfills our data needs, we just force an
	 * alignment to happen.  Afterwards, we have enough space.
	 */
	// 如果因 misalign 错位的空间比 datlen 需要的长度大，只需强制进行对齐，完了就有足够的空间了
	if (buf->misalign >= datlen) {
		evbuffer_align(buf);
	} else {
		// 如果对齐后空间也不够，则需要进行扩容逻辑
		void *newbuf;
		size_t length = buf->totallen;
		// 最小 256 字节，如果空间不够，则翻倍后再判断，不够则继续翻倍
		if (length < 256)
			length = 256;
		while (length < need)
			length <<= 1;
		// 扩容时，若内存区块起点和实际内存区块起点不一致（即misalign不为0），则进行一次对齐
		if (buf->orig_buffer != buf->buffer)
			evbuffer_align(buf);
		// 将原内存块的内容复制到新内存块中，并返回新内存块的指针
		if ((newbuf = realloc(buf->buffer, length)) == NULL)
			return (-1);
		// 将 buf 的 orig_buffer 和 buffer 都修改到新的内存块指针
		buf->orig_buffer = buf->buffer = newbuf;
		// 修改内存块总大小
		buf->totallen = length;
	}

	return (0);
}

/**
 * @brief 将一段二进制数据添加到 evbuffer 中，完了调用回调
 * 
 * @param buf 要添加到的 evbuffer
 * @param data 要添加的数据
 * @param datlen 要添加的数据长度
 * @return int 0：成功 -1：失败
 */
int
evbuffer_add(struct evbuffer *buf, void *data, size_t datlen)
{
	// 计算数据添加后的数据长度：数据偏移量 + 当前数据长度 + 写入数据长度
	size_t need = buf->misalign + buf->off + datlen;
	// 保存当前数据长度
	size_t oldoff = buf->off;
	// 若当前内存块总长度小于添加后的长度，说明需要扩容了，调用一下扩容函数
	if (buf->totallen < need) {
		if (evbuffer_expand(buf, datlen) == -1)
			return (-1);
	}
	// （若需要扩容）扩容成功后，将 data 为起点，datlen 长度的数据，拷贝到 buf->buffer中
	memcpy(buf->buffer + buf->off, data, datlen);
	// 数据拷贝完成后，修改 buf 的 off，加上目标数据长度
	buf->off += datlen;
	// 如果添加的数据长度 > 0，而且 buf 有回调函数，则调用回调函数，将 buf、添加前 off、添加后 off、cbarg 传入
	// 添加完毕后调用回调函数
	if (datlen && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (0);
}

/**
 * @brief 把 evbuffer 的实际数据清理一段
 * 
 * @param buf 要被清空的 evbuffer
 * @param len 要清空的实际数据长度
 */
void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;
	// 如果传入的 len 比实际长度大，说明全部清完，直接将实际数据的起点设置到内存块头部，实际偏移量设置为0，错位偏移量设置为0，然后调用回调
	if (len >= buf->off) {
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;
	}
	// 否则只是将 buffer 的指针从实际数据块的头部移动到尾部，并将 misalign 添加一个被清空的长度
	// 实际上就是把之前的实际数据忽略了，把内存块的错位偏移量往前移动
	// 这样做能避免一次内存拷贝，可以等到后续内存块需要扩容时再做内存拷贝操作
	buf->buffer += len;
	buf->misalign += len;
	// 实际数据长度减去被清理
	buf->off -= len;

 done:
	// 清理完成，调用回调函数
	if (buf->off != oldoff && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

}

/*
 * Reads data from a file descriptor into a buffer.
 */

#define EVBUFFER_MAX_READ	4096

/**
 * @brief 从一个文件描述符中读取数据到 evbuffer 中
 * 
 * @param buf 目标 evbuffer
 * @param fd 被读取的文件描述符
 * @param howmuch 读取的数据量，穿 -1 则读取 4k
 * @return int 实际读取的数据长度，或者 read 返回的 0/-1
 */
int
evbuffer_read(struct evbuffer *buf, int fd, int howmuch)
{
	u_char *p;
	size_t oldoff = buf->off;
	int n = EVBUFFER_MAX_READ;
#ifdef WIN32
	DWORD dwBytesRead;
#endif

#ifdef FIONREAD
	if (ioctl(fd, FIONREAD, &n) == -1 || n == 0)
		n = EVBUFFER_MAX_READ;
#endif	
	// 最多一次读取4k
	if (howmuch < 0 || howmuch > n)
		howmuch = n;

	// 尝试扩容 evbuffer
	if (evbuffer_expand(buf, howmuch) == -1)
		return (-1);

	/* We can append new data at this point */
	// 读取到的数据写入的起点地址
	p = buf->buffer + buf->off;

#ifndef WIN32
	// 读取数据
	n = read(fd, p, howmuch);
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
#else
	n = ReadFile((HANDLE)fd, p, howmuch, &dwBytesRead, NULL);
	if (n == 0)
		return (-1);
	if (dwBytesRead == 0)
		return (0);
	n = dwBytesRead;
#endif
	// 修改实际数据长度并调用回调
	buf->off += n;

	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (n);
}

/**
 * @brief 将 evbuffer 的数据写入 fd，然后清理
 * 
 * @param buffer 要写入fd的evbuffer
 * @param fd 要写入的目标fd
 * @return int 写入的数据长度，或者 write 的返回
 */
int
evbuffer_write(struct evbuffer *buffer, int fd)
{
	int n;
#ifdef WIN32
	DWORD dwBytesWritten;
#endif

#ifndef WIN32
	n = write(fd, buffer->buffer, buffer->off);
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
#else
	n = WriteFile((HANDLE)fd, buffer->buffer, buffer->off, &dwBytesWritten, NULL);
	if (n == 0)
		return (-1);
	if (dwBytesWritten == 0)
		return (0);
	n = dwBytesWritten;
#endif
	evbuffer_drain(buffer, n);

	return (n);
}

/**
 * @brief 从 evbuffer 中查找一段字符
 * 
 * @param buffer 目标 evbuffer
 * @param what 查找的内容起点
 * @param len 查找内容的目标长度
 * @return u_char* 
 */
u_char *
evbuffer_find(struct evbuffer *buffer, u_char *what, size_t len)
{
	size_t remain = buffer->off;
	u_char *search = buffer->buffer;
	u_char *p;

	while ((p = memchr(search, *what, remain)) != NULL && remain >= len) {
		if (memcmp(p, what, len) == 0)
			return (p);

		search = p + 1;
		remain = buffer->off - (size_t)(search - buffer->buffer);
	}

	return (NULL);
}

/**
 * @brief 为 evbuffer 设置回调函数
 * 
 * @param buffer 目标 evbuffer
 * @param cb 回调函数
 * @param cbarg 传递给回调函数的参数
 */
void evbuffer_setcb(struct evbuffer *buffer,
    void (*cb)(struct evbuffer *, size_t, size_t, void *),
    void *cbarg)
{
	buffer->cb = cb;
	buffer->cbarg = cbarg;
}
