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
	int i, j;

	for (j = 0; j < LOOPS; j++) {
		usleep(150);
		for (i = 0; i < NPIPES; i++) {
			int ret;

			ret = write(d->pipes[i][1], "foo", 3);
			if (ret < 0)
				perror("write");
		}
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

static int test_race_epoll_only(void)
{
    struct d d;
    pthread_t thread;
    struct epoll_event ev, out[NPIPES];
    int efd = -1, ret = 1;
    size_t total_drained = 0;

    fprintf(stderr, "[ctrl] enter\n"); fflush(stderr);

    for (int i = 0; i < NPIPES; i++) {
        if (pipe(d.pipes[i]) < 0) {
            perror("[ctrl] pipe");
            return 1;
        }
    }

    efd = epoll_create1(0);
    if (efd < 0) {
        perror("[ctrl] epoll_create1");
        goto out;
    }

    for (int i = 0; i < NPIPES; i++) {
        ev.events = EPOLLIN;
        ev.data.fd = d.pipes[i][0];
        if (epoll_ctl(efd, EPOLL_CTL_ADD, d.pipes[i][0], &ev) < 0) {
            perror("[ctrl] epoll_ctl");
            goto out;
        }
    }

    if (pthread_create(&thread, NULL, thread_fn, &d) != 0) {
        perror("[ctrl] pthread_create");
        goto out;
    }

    for (int j = 0; j < LOOPS; j++) {
        fprintf(stderr, "[ctrl] loop=%d waiting...\n", j);
        fflush(stderr);

        int nr = epoll_wait(efd, out, NPIPES, -1);  /* block like the uring test */
        if (nr < 0) {
            perror("[ctrl] epoll_wait");
            goto join_out;
        }

        fprintf(stderr, "[ctrl] loop=%d epoll returned nr=%d\n", j, nr);
        fflush(stderr);

        /* drain exactly what epoll reported */
        for (int i = 0; i < nr; i++) {
            char tmp[32];
            int r = read(out[i].data.fd, tmp, sizeof(tmp));
            if (r < 0)
                perror("[ctrl] read");
        }
        total_drained += (size_t)nr;
    }

    ret = 0;
    fprintf(stderr, "[ctrl] done OK (total_drained=%zu)\n", total_drained);
    fflush(stderr);

join_out:
    pthread_join(thread, NULL);
out:
    if (ret)
        fprintf(stderr, "[ctrl] exit rc=%d (total_drained=%zu)\n", ret, total_drained), fflush(stderr);
    if (efd >= 0) close(efd);
    for (int i = 0; i < NPIPES; i++) {
        close(d.pipes[i][0]);
        close(d.pipes[i][1]);
    }
    return ret;
}

static int test_race(int flags)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	struct d d;
	struct epoll_event ev;
	struct epoll_event out[NPIPES];
	pthread_t thread;
	int i, j, efd, ret;
	void *tret;

	ret = t_create_ring(32, &ring, flags);
	if (ret == T_SETUP_SKIP) {
		return 0;
	} else if (ret != T_SETUP_OK) {
		fprintf(stderr, "ring create failed %x -> %d\n", flags, ret);
		return 1;
	}

	for (i = 0; i < NPIPES; i++) {
		if (pipe(d.pipes[i]) < 0) {
			perror("pipe");
			return 1;
		}
	}

	efd = epoll_create1(0);
	if (efd < 0) {
		perror("epoll_create");
		return T_EXIT_FAIL;
	}

	for (i = 0; i < NPIPES; i++) {
		ev.events = EPOLLIN;
		ev.data.fd = d.pipes[i][0];
		ret = epoll_ctl(efd, EPOLL_CTL_ADD, d.pipes[i][0], &ev);
		if (ret < 0) {
			perror("epoll_ctl");
			return T_EXIT_FAIL;
		}
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_epoll_wait(sqe, efd, out, NPIPES, 0);
	io_uring_submit(&ring);

	pthread_create(&thread, NULL, thread_fn, &d);

	for (j = 0; j < LOOPS; j++) {
		io_uring_submit_and_wait(&ring, 1);

		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret) {
			fprintf(stderr, "wait %d\n", ret);
			return 1;
		}
		if (cqe->res < 0) {
			fprintf(stderr, "race res %d\n", cqe->res);
			return 1;
		}
		prune(out, cqe->res);
		io_uring_cqe_seen(&ring, cqe);
		usleep(100);
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_epoll_wait(sqe, efd, out, NPIPES, 0);
	}

	pthread_join(thread, &tret);

	for (i = 0; i < NPIPES; i++) {
		close(d.pipes[i][0]);
		close(d.pipes[i][1]);
	}
	close(efd);
	io_uring_queue_exit(&ring);
	return 0;
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

	fprintf(stderr, "[ctrl] epoll-only start\n"); fflush(stderr);
	int ctrl = test_race_epoll_only();
	fprintf(stderr, "[ctrl] epoll-only rc=%d\n", ctrl); fflush(stderr);

	if (ctrl) {
    	fprintf(stderr, "epoll-only control failed/hung -> not an io_uring-specific issue\n");
    	return T_EXIT_FAIL;
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
