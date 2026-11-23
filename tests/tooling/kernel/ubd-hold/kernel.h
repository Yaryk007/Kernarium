/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Just enough of the kernel for the UBD driver's held-request functions, which
 * test_darwin_ubd_hold.py cuts out of the series' topic and compiles with this
 * header in front. Everything the functions call is declared here and defined
 * by driver.cpp, which scripts the host's answers.
 */
#ifndef UBD_HOLD_KERNEL_H
#define UBD_HOLD_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t u64;
typedef unsigned char blk_status_t;

#define BLK_STS_OK 0
#define BLK_STS_NOTSUPP 1
#define BLK_STS_NOSPC 3
#define BLK_STS_IOERR 10

enum req_op {
	REQ_OP_READ,
	REQ_OP_WRITE,
	REQ_OP_FLUSH,
	REQ_OP_DISCARD,
	REQ_OP_WRITE_ZEROES,
};

struct request_queue;

struct request {
	enum req_op op;
	struct request_queue *q;
};
#define req_op(request) ((request)->op)

struct io_desc;

/* The members of the driver's request that the functions read or write. */
struct io_thread_req {
	struct request *req;
	bool direct;
	u64 offset;
	blk_status_t error;
};

struct gendisk {
	char disk_name[32];
};

struct request_queue {
	struct gendisk *disk;
};

struct ubd {
	struct gendisk *disk;
};

#define HZ 1000
extern unsigned long jiffies;
#define msecs_to_jiffies(ms) ((unsigned long)(ms))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))

void test_print(const char *level, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
#define pr_warn(...) test_print("warn", __VA_ARGS__)
#define pr_info(...) test_print("info", __VA_ARGS__)

/* The wait queue is only a name here; the wait is the driver's to script. */
typedef int wait_queue_head_t;
void test_wait(unsigned long timeout);
#define wait_event_idle_timeout(queue, condition, timeout) \
	((void)&(queue), (void)sizeof(condition), test_wait(timeout), 0L)

/* One attempt at a segment, and at a flush, as the real driver names them. */
void darwin_ubd_do_segment(struct ubd *dev, struct io_thread_req *io_req,
			   char *buffer, unsigned long length,
			   unsigned long byte_offset);
void do_io(struct io_thread_req *req, struct io_desc *desc);

#endif
