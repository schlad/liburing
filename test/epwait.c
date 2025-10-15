/* SPDX-License-Identifier: MIT */
/*
 * Test various invocations of IORING_OP_EPOLL_WAIT
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <poll.h>
#include <string.h>
#include <pthread.h>
#include "liburing.h"
#include "helpers.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned long wr_ticks = 0;   /* incremented in writer */
static _Atomic unsigned long wr_bytes = 0;   /* incremented in writer */

static _Atomic int wd_run = 0;               /* watchdog running flag */
static _Atomic int wd_waiting = 0;           /* main is in wait() */
static int wd_efd = -1;                      /* epoll fd for probe */

static int fds[2][2];
static int no_epoll_wait;

static int test_ready(struct io_uring *ring, int efd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct epoll_event out[2];
	char tmp[16];
	int i, ret;

	for (i = 0; i < 2; i++) {
		ret = write(fds[i][1], "foo", 3);
		if (ret < 0)
			perror("write");
	}

	memset(out, 0, sizeof(out));
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_epoll_wait(sqe, efd, out, 2, 0);
	sqe->user_data = 1;
	io_uring_submit(ring);

	for (i = 0; i < 1; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			return 1;
		}
		if (cqe->res == -EINVAL) {
			no_epoll_wait = 1;
			return 0;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	for (i = 0; i < 2; i++) {
		ret = read(out[i].data.fd, tmp, sizeof(tmp));
		if (ret < 0)
			perror("read");
	}

	return 0;
}

static int test_not_ready(struct io_uring *ring, int efd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct epoll_event out[2];
	int i, ret, nr;
	char tmp[16];

	memset(out, 0, sizeof(out));
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_epoll_wait(sqe, efd, out, 2, 0);
	sqe->user_data = 1;
	io_uring_submit(ring);

	for (i = 0; i < 2; i++) {
		usleep(10000);
		ret = write(fds[i][1], "foo", 3);
		if (ret < 0)
			perror("write");
	}

	nr = 0;
	for (i = 0; i < 1; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			break;
		}
		nr = cqe->res;
		io_uring_cqe_seen(ring, cqe);
		if (nr < 0) {
			fprintf(stderr, "not ready res %d\n", nr);
			return 1;
		}
	}

	for (i = 0; i < nr; i++) {
		ret = read(out[i].data.fd, tmp, sizeof(tmp));
		if (ret < 0)
			perror("read");
	}
	
	return 0;
}

static int test_del(struct io_uring *ring, int efd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct epoll_event ev, out[2];
	int i, ret;
	char tmp[16];

	memset(out, 0, sizeof(out));
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_epoll_wait(sqe, efd, out, 2, 0);
	sqe->user_data = 1;
	io_uring_submit(ring);

	ev.events = EPOLLIN;
	ev.data.fd = fds[0][0];
	ret = epoll_ctl(efd, EPOLL_CTL_DEL, fds[0][0], &ev);
	if (ret < 0) {
		perror("epoll_ctl");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		ret = write(fds[i][1], "foo", 3);
		if (ret < 0)
			perror("write");
	}

	for (i = 0; i < 1; i++) {
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	for (i = 0; i < 2; i++) {
		ret = read(fds[i][0], tmp, sizeof(tmp));
		if (ret < 0)
			perror("read");
	}

	ev.events = EPOLLIN;
	ev.data.fd = fds[0][0];
	ret = epoll_ctl(efd, EPOLL_CTL_ADD, fds[0][0], &ev);
	if (ret < 0) {
		perror("epoll_ctl");
		return T_EXIT_FAIL;
	}

	return 0;
}

static int test_remove(struct io_uring *ring, int efd)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct epoll_event out[2];
	int i, ret;

	memset(out, 0, sizeof(out));
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_epoll_wait(sqe, efd, out, 2, 0);
	sqe->user_data = 1;
	io_uring_submit(ring);

	close(efd);

	usleep(10000);
	for (i = 0; i < 2; i++) {
		ret = write(fds[i][1], "foo", 3);
		if (ret < 0)
			perror("write");
	}

	for (i = 0; i < 1; i++) {
		ret = io_uring_peek_cqe(ring, &cqe);
		if (ret == -EAGAIN) {
			break;
		} else if (ret) {
			fprintf(stderr, "wait_cqe ret = %d\n", ret);
			break;
		}
		io_uring_cqe_seen(ring, cqe);
	}

	return 0;
}

#define LOOPS	500
#define NPIPES	8

struct d {
	int pipes[NPIPES][2];
};

static void *thread_fn(void *data)
{
    struct d *d = data;
    for (int j = 0; j < LOOPS; j++) {
        usleep(150);
        for (int i = 0; i < NPIPES; i++) {
            int ret = write(d->pipes[i][1], "foo", 3);
            if (ret >= 0)
                atomic_fetch_add(&wr_bytes, (unsigned long)ret);
            else
                perror("write");
        }
        atomic_fetch_add(&wr_ticks, 1UL);
    }
    return NULL;
}

static void *watchdog_fn(void *arg)
{
    (void)arg;
    unsigned long last_ticks = 0, last_bytes = 0;
    struct timespec start = {0}, now = {0};
    int have_window = 0;

    while (atomic_load(&wd_run)) {
        if (atomic_load(&wd_waiting)) {
            unsigned long t = atomic_load(&wr_ticks);
            unsigned long b = atomic_load(&wr_bytes);
            if (!have_window) {
                last_ticks = t; last_bytes = b;
                clock_gettime(CLOCK_MONOTONIC, &start);
                have_window = 1;
            } else {
                clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - start.tv_sec) * 1000L +
                          (now.tv_nsec - start.tv_nsec) / 1000000L;
                if (ms >= 1000 && t == last_ticks && b == last_bytes) {
                    /* probe epoll readiness without blocking */
                    struct epoll_event probe[NPIPES];
                    int n = epoll_wait(wd_efd, probe, NPIPES, 0);
                    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
                    fprintf(stderr,
                        "STARVATION(BLOCKED): waited ~%ld ms, writer unchanged "
                        "(ticks=%lu, bytes=%lu), epoll n=%d, CPUs=%ld\n",
                        ms, t, b, n, ncpu);
                    /* reset window so we don’t spam */
                    have_window = 0;
                }
            }
        } else {
            have_window = 0; /* not waiting now */
        }
        usleep(500 * 1000); /* 500 ms */
    }
    return NULL;
}

static void prune(struct epoll_event *evs, int nr)
{
	char tmp[32];
	int i, ret;

	for (i = 0; i < nr; i++) {
		ret = read(evs[i].data.fd, tmp, sizeof(tmp));
		if (ret < 0)
			perror("read");
	}
}

static inline long diff_ms(struct timespec a, struct timespec b)
{
    return (long)((b.tv_sec - a.tv_sec) * 1000 +
                  (b.tv_nsec - a.tv_nsec) / 1000000);
}

static int test_race(int flags)
{
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    struct d d;
    struct epoll_event ev;
    struct epoll_event out[2][NPIPES];
    pthread_t thread, wd;
    int i, j, efd, ret;
    void *tret;

    ret = t_create_ring(32, &ring, flags);
    if (ret == T_SETUP_SKIP) return 0;
    if (ret != T_SETUP_OK) {
        fprintf(stderr, "ring create failed %x -> %d\n", flags, ret);
        return 1;
    }

    for (i = 0; i < NPIPES; i++) {
        if (pipe(d.pipes[i]) < 0) {
            perror("pipe");
            io_uring_queue_exit(&ring);
            return 1;
        }
    }

    efd = epoll_create1(0);
    if (efd < 0) {
        perror("epoll_create");
        for (i = 0; i < NPIPES; i++) { close(d.pipes[i][0]); close(d.pipes[i][1]); }
        io_uring_queue_exit(&ring);
        return T_EXIT_FAIL;
    }
    wd_efd = efd; /* watchdog will probe this fd */

    for (i = 0; i < NPIPES; i++) {
        ev.events = EPOLLIN;
        ev.data.fd = d.pipes[i][0];
        ret = epoll_ctl(efd, EPOLL_CTL_ADD, d.pipes[i][0], &ev);
        if (ret < 0) {
            perror("epoll_ctl");
            for (i = 0; i < NPIPES; i++) { close(d.pipes[i][0]); close(d.pipes[i][1]); }
            close(efd);
            io_uring_queue_exit(&ring);
            return T_EXIT_FAIL;
        }
    }

    memset(out, 0, sizeof(out));

    /* arm TWO waits up front; tag user_data = 0 / 1 */
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_epoll_wait(sqe, efd, out[0], NPIPES, 0);
    sqe->user_data = 0;

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_epoll_wait(sqe, efd, out[1], NPIPES, 0);
    sqe->user_data = 1;

    io_uring_submit(&ring);

    /* start writer + watchdog */
    pthread_create(&thread, NULL, thread_fn, &d);
    atomic_store(&wd_run, 1);
    pthread_create(&wd, NULL, watchdog_fn, NULL);

    for (j = 0; j < LOOPS; j++) {
        atomic_store(&wd_waiting, 1);
        ret = io_uring_wait_cqe(&ring, &cqe);
        atomic_store(&wd_waiting, 0);
        if (ret) {
            fprintf(stderr, "wait %d\n", ret);
            goto fail;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "race res %d\n", cqe->res);
            io_uring_cqe_seen(&ring, cqe);
            goto fail;
        }

        /* which buffer got filled */
        unsigned buf_id = (unsigned)cqe->user_data;
        if (buf_id > 1) buf_id = 0;

        /* drain exactly cqe->res */
        for (i = 0; i < cqe->res; i++) {
            char tmp[32];
            int fd = out[buf_id][i].data.fd;
            if (fd >= 0) {
                int r = read(fd, tmp, sizeof(tmp));
                if (r < 0) perror("read");
            }
        }
        memset(out[buf_id], 0, sizeof(struct epoll_event) * (unsigned)cqe->res);

        io_uring_cqe_seen(&ring, cqe);

        /* immediately re-arm the same buffer */
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_epoll_wait(sqe, efd, out[buf_id], NPIPES, 0);
        sqe->user_data = buf_id;
        io_uring_submit(&ring);
    }

    atomic_store(&wd_run, 0);
    pthread_join(wd, &tret);
    pthread_join(thread, &tret);

    for (i = 0; i < NPIPES; i++) {
        close(d.pipes[i][0]);
        close(d.pipes[i][1]);
    }
    close(efd);
    io_uring_queue_exit(&ring);
    return 0;

fail:
    atomic_store(&wd_run, 0);
    pthread_join(wd, &tret);
    pthread_join(thread, &tret);
    for (i = 0; i < NPIPES; i++) {
        close(d.pipes[i][0]);
        close(d.pipes[i][1]);
    }
    close(efd);
    io_uring_queue_exit(&ring);
    return 1;
}

static int test(int flags)
{
	struct epoll_event ev = { };
	struct io_uring ring;
	int epollfd, ret, i;

	ret = t_create_ring(8, &ring, flags);
	if (ret == T_SETUP_SKIP) {
		return T_EXIT_SKIP;
	} else if (ret != T_SETUP_OK) {
		fprintf(stderr, "ring create failed %x -> %d\n", flags, ret);
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		if (pipe(fds[i]) < 0) {
			perror("pipe");
			return T_EXIT_FAIL;
		}
	}

	epollfd = epoll_create1(0);
	if (epollfd < 0) {
		perror("epoll_create");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		ev.events = EPOLLIN;
		ev.data.fd = fds[i][0];
		ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fds[i][0], &ev);
		if (ret < 0) {
			perror("epoll_ctl");
			return T_EXIT_FAIL;
		}
	}

	ret = test_ready(&ring, epollfd);
	if (ret) {
		fprintf(stderr, "test_ready failed\n");
		return T_EXIT_FAIL;
	}
	if (no_epoll_wait)
		return T_EXIT_SKIP;

	ret = test_not_ready(&ring, epollfd);
	if (ret) {
		fprintf(stderr, "test_not_ready failed\n");
		return T_EXIT_FAIL;
	}

	ret = test_del(&ring, epollfd);
	if (ret) {
		fprintf(stderr, "test_del failed\n");
		return T_EXIT_FAIL;
	}

	/* keep last */
	ret = test_remove(&ring, epollfd);
	if (ret) {
		fprintf(stderr, "test_remove failed\n");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < 2; i++) {
		close(fds[i][0]);
		close(fds[i][1]);
	}

	ret = test_race(flags);
	if (ret) {
		fprintf(stderr, "test_race failed\n");
		return T_EXIT_FAIL;
	}

	close(epollfd);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return T_EXIT_SKIP;

	ret = test(0);
	if (ret == T_EXIT_SKIP)
		return T_EXIT_SKIP;
	else if (ret) {
		fprintf(stderr, "test 0 failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(IORING_SETUP_DEFER_TASKRUN|IORING_SETUP_SINGLE_ISSUER);
	if (ret) {
		fprintf(stderr, "test defer failed\n");
		return T_EXIT_FAIL;
	}

	ret = test(IORING_SETUP_SQPOLL);
	if (ret) {
		fprintf(stderr, "test sqpoll failed\n");
		return T_EXIT_FAIL;
	}

	return T_EXIT_PASS;
}
