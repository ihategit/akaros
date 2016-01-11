/* Copyright (c) 2016 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * select()
 *
 * Our select() is super spurious and will only work with apps that use
 * non-blocking I/O.
 *
 * Under the hood, our select() is implemented with epoll (and under that, FD
 * taps).  Those can only detect edges (e.g. a socket becomes readable).
 *
 * The problem is that we want to detect a level status (e.g. socket is
 * readable) with an edge event (e.g. socket *becomes* readable).  To do this,
 * when someone initially selects, the FD gets tracked with epoll and we
 * immediately return saying the FD is ready for whatever they asked for.  This
 * is usually not true, and the application will need to poll all of its FDs
 * once after the initial select() call.  Subsequent selects() will still be
 * tracking the FD in the epoll set.  If any edge events that come after the
 * poll (which eventually returns EAGAIN) will be caught by epoll, and a
 * subsequent select will wake up (or never block in the first place) due to the
 * reception of that edge event.
 *
 * We maintain one FD set per program.  It tracks *any* FD being tracked by
 * *any* select call.  Regardless of whether the user asked for
 * read/write/except, the FD gets watched for anything until it closes.  This
 * will result in spurious wakeups.
 *
 * One issue with the global FD set is that one thread may consume the epoll
 * events intended for another thread.  To get around this, only one thread is
 * the actual epoller, and the others block on a mutex.  An alternative is to
 * use a per-thread FD set, using TLS, but not every 2LS uses TLS, and
 * performance is not a concern for code using select().
 *
 * Notes:
 * - pselect might be racy
 * - if the user has no read/write/except sets, we won't wait.  some users of
 *   select use it as a timer only.  if that comes up, we can expand this.
 * - if you epoll or FD tap an FD, then try to use select on it, you'll get an
 *   error (only one tap per FD).  select() only knows about the FDs in its set.
 */

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <ros/common.h>
#include <parlib/uthread.h>
#include <sys/close_cb.h>
#include <sys/epoll.h>
#include <malloc.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>

static int epoll_fd;
static fd_set all_fds;
static uth_mutex_t fdset_mtx;
static struct uthread *owner;
static uth_mutex_t sleep_mtx;

static bool fd_is_set(unsigned int fd, fd_set *set)
{
	if (fd > FD_SETSIZE)
		return FALSE;
	if (!set)
		return FALSE;
	return FD_ISSET(fd, set);
}

static void select_fd_closed(int fd)
{
	/* Slightly racy, but anything concurrently added will be closed later, and
	 * after it is_set. */
	if (!fd_is_set(fd, &all_fds))
		return;
	/* We just need to stop tracking FD.  We do not need to remove it from the
	 * epoll set, since that will happen automatically on close(). */
	uth_mutex_lock(fdset_mtx);
	FD_CLR(fd, &all_fds);
	uth_mutex_unlock(fdset_mtx);
}

static void select_init(void)
{
	static struct close_cb select_close_cb = {.func = select_fd_closed};

	register_close_cb(&select_close_cb);
	epoll_fd = epoll_create(FD_SETSIZE);
	if (epoll_fd < 0) {
		perror("select failed epoll_create");
		exit(-1);
	}
	fdset_mtx = uth_mutex_alloc();
	sleep_mtx = uth_mutex_alloc();
}

static int select_tv_to_ep_timeout(struct timeval *tv)
{
	if (!tv)
		return -1;
	return tv->tv_sec * 1000 + DIV_ROUND_UP(tv->tv_usec, 1000);
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
	bool changed_set = FALSE;
	struct epoll_event ep_ev;
	struct epoll_event *ep_results;
	int ep_timeout = select_tv_to_ep_timeout(timeout);

	run_once(select_init());
	/* good thing nfds is a signed int... */
	if (nfds < 0) {
		errno = EINVAL;
		return -1;
	}
	uth_mutex_lock(fdset_mtx);
	for (int i = 0; i < nfds; i++) {
		if ((fd_is_set(i, readfds) || fd_is_set(i, writefds) ||
		    fd_is_set(i, exceptfds)) && !fd_is_set(i, &all_fds)) {

			changed_set = TRUE;
			FD_SET(i, &all_fds);
			/* FDs that we track for *any* reason with select will be
			 * tracked for *all* reasons with epoll. */
			ep_ev.events = EPOLLET | EPOLLIN | EPOLLOUT | EPOLLHUP |
			               EPOLLERR;
			ep_ev.data.fd = i;
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, i, &ep_ev)) {
				/* We might have failed because we tried to set up too many
				 * FD tap types.  Listen FDs, for instance, can only be
				 * tapped for READABLE and HANGUP.  Let's try for one of
				 * those. */
				if (errno == ENOSYS) {
					ep_ev.events = EPOLLET | EPOLLIN | EPOLLHUP;
					if (!epoll_ctl(epoll_fd, EPOLL_CTL_ADD, i, &ep_ev))
						continue;
				}
				/* Careful to unlock before calling perror.  perror calls
				 * close, which calls our CB, which grabs the lock. */
				uth_mutex_unlock(fdset_mtx);
				perror("select epoll_ctl failed");
				return -1;
			}
		}
	}
	uth_mutex_unlock(fdset_mtx);
	/* Since we just added some FD to our tracking set, we don't know if its
	 * readable or not.  We'll only catch edge-triggered changes in the future.
	 * We can spuriously tell the user all FDs are ready, and next time they
	 * can block until there is edge activity. */
	if (changed_set)
		return nfds;
	/* Since there is a global epoll set, we could have multiple threads
	 * epolling at a time and one thread could consume the events that should
	 * wake another thread.  We don't know when the 'other' thread last polled,
	 * so we'll need to assume its event was consumed and just return.  If a
	 * thread selects again and no other thread has since selected, then we know
	 * no one consumed the events.  We use an 'owner' to track which thread most
	 * recently selected.  We use a mutex so that the extra threads sleep. */
	uth_mutex_lock(sleep_mtx);
	if (owner != current_uthread) {
		/* Could thrash, if we fight with another uth for owner */
		owner = current_uthread;
		uth_mutex_unlock(sleep_mtx);
		return nfds;
	}
	/* Need to check for up to FD_SETSIZE - nfds isn't the size of all FDs
	 * tracked; it's the size of only our current select call */
	ep_results = malloc(sizeof(struct epoll_event) * FD_SETSIZE);
	if (!ep_results) {
		uth_mutex_unlock(sleep_mtx);
		errno = ENOMEM;
		return -1;
	}
	/* Don't care which ones were set; we'll just tell the user they all were
	 * set.  If they can't handle that, this whole plan won't work. */
	epoll_wait(epoll_fd, ep_results, FD_SETSIZE, ep_timeout);
	uth_mutex_unlock(sleep_mtx);
	free(ep_results);
	/* TODO: consider updating timeval.  It's not mandatory (POSIX). */
	return nfds;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask)
{
	int ready;
	sigset_t origmask;
	struct timeval local_tv, *tv = &local_tv;

	if (!timeout) {
		tv = 0;
	} else {
		tv->tv_sec = timeout->tv_sec;
		tv->tv_usec = DIV_ROUND_UP(timeout->tv_nsec, 1000);
	}
	/* TODO: this is probably racy */
	sigprocmask(SIG_SETMASK, sigmask, &origmask);
	ready = select(nfds, readfds, writefds, exceptfds, tv);
	sigprocmask(SIG_SETMASK, &origmask, NULL);
	return ready;
}
