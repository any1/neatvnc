/*
 * Copyright (c) 2023 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include "stream.h"

#include <unistd.h>

struct stream;

int stream_tcp_init(struct stream* self, int fd, stream_event_fn on_event,
		void* userdata);
int stream_tcp_close(struct stream* self);
void stream_tcp_destroy(struct stream* self);
ssize_t stream_tcp_read(struct stream* self, void* dst, size_t size);
int stream_tcp_send(struct stream* self, struct rcbuf* payload,
                stream_req_fn on_done, void* userdata);
int stream_tcp_send_first(struct stream* self, struct rcbuf* payload);
void stream_tcp_exec_and_send(struct stream* self,
		stream_exec_fn exec_fn, void* userdata);
int stream_tcp_install_cipher(struct stream* self,
		struct crypto_cipher* cipher);
