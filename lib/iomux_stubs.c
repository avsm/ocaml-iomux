#ifdef IS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#else
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <errno.h>

#include <caml/bigarray.h>
#include <caml/memory.h>
#include <caml/mlvalues.h>
#include <caml/unixsupport.h>
#include <caml/signals.h>

#include "config.h"

#ifdef IS_WINDOWS
/* Windows uses WSAPOLLFD which is compatible with pollfd layout */
typedef WSAPOLLFD pollfd_t;
typedef ULONG nfds_t;
#else
typedef struct pollfd pollfd_t;
#endif

/* only defined in the runtime with CAML_INTERNALS */
CAMLextern int caml_convert_signal_number (int);

#ifdef IS_WINDOWS
/* WSA initialization state */
static int wsa_initialized = 0;

/* Initialize Winsock if not already initialized */
static void
ensure_wsa_initialized(void)
{
	if (!wsa_initialized) {
		WSADATA wsaData;
		int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (result != 0) {
			/* WSAStartup failed */
			errno = ENOSYS;
			uerror("WSAStartup", Nothing);
		}
		wsa_initialized = 1;
	}
}
#endif

/*
 * Poll
 */

value
caml_iomux_poll(value v_fds, value v_nfds, value v_timo)
{
	CAMLparam3(v_fds, v_nfds, v_timo);
	pollfd_t *fds;
	nfds_t nfds;
	int timo;
	int r;

#ifdef IS_WINDOWS
	ensure_wsa_initialized();
#endif

	fds = Caml_ba_data_val(v_fds);
	nfds = Int_val(v_nfds);
	timo = Int_val(v_timo);

	caml_enter_blocking_section();
#ifdef IS_WINDOWS
	r = WSAPoll(fds, nfds, timo);
#else
	r = poll(fds, nfds, timo);
#endif
	caml_leave_blocking_section();
	if (r == -1) /* this allocs */
		uerror("poll", Nothing);

	CAMLreturn(Val_int(r));
}

#ifdef HAS_PPOLL
static void
decode_sigset(value vset, sigset_t * set)
{
	sigemptyset(set);
	for (/*nothing*/; vset != Val_emptylist; vset = Field(vset, 1)) {
		int sig = caml_convert_signal_number(Int_val(Field(vset, 0)));
		sigaddset(set, sig);
	}
}
#endif

#define S_IN_NS 1000000000LL
value
caml_iomux_ppoll(value v_fds, value v_nfds, value v_timo, value v_sigmask)
{
#ifdef HAS_PPOLL
	CAMLparam4(v_fds, v_nfds, v_timo, v_sigmask);
	struct pollfd *fds;
	struct timespec *timo;
	struct timespec ts;
	sigset_t *psigmask, sigmask;
	nfds_t nfds;
	int64_t timo64;
	int r;

	fds = Caml_ba_data_val(v_fds);
	nfds = Int_val(v_nfds);
	timo64 = Int64_val(v_timo);
	if (timo64 == -1LL)
		timo = NULL;
	else {
		ts.tv_sec = (time_t)(timo64 / S_IN_NS);
		ts.tv_nsec = (time_t)(timo64 % S_IN_NS);
		timo = &ts;
	}

	if (v_sigmask == Val_emptylist)
		psigmask = NULL;
	else {
		decode_sigset(v_sigmask, &sigmask);
		psigmask = &sigmask;
	}

	caml_enter_blocking_section();
	r = ppoll(fds, nfds, timo, psigmask);
	caml_leave_blocking_section();
	if (r == -1) /* this allocs */
		uerror("ppoll", Nothing);

	CAMLreturn(Val_int(r));
#else /* HAS_PPOLL */
	errno = ENOSYS;
	uerror("ppoll", Nothing);
#endif /* HAS_PPOLL */
}
#undef S_IN_NS

#define pollfd_of_index(vfds, vindex)					\
	((pollfd_t *)Caml_ba_data_val(vfds) + (Int_val (vindex)))

value /* noalloc */
caml_iomux_poll_set_index(value v_fds, value v_index, value v_fd, value v_events)
{
	pollfd_t *pfd = pollfd_of_index(v_fds, v_index);

	pfd->fd = Int_val(v_fd);
	pfd->events = Int_val(v_events);

	return (Val_unit);
}

value
caml_iomux_poll_init(value v_fds, value v_maxfds)
{
	CAMLparam2(v_fds, v_maxfds);
	pollfd_t *pfd = pollfd_of_index(v_fds, Val_int(0));
	int maxfds = Int_val(v_maxfds);
	int i;

	for (i = 0; i < maxfds; i++, pfd++) {
#ifdef IS_WINDOWS
		pfd->fd = INVALID_SOCKET;
#else
		pfd->fd = -1;
#endif
		pfd->events = 0;
	}

	CAMLreturn(Val_unit);
}


value /* noalloc */
caml_iomux_poll_get_revents(value v_fds, value v_index)
{
	pollfd_t *pfd = pollfd_of_index(v_fds, v_index);

	return (Val_int(pfd->revents));
}

value /* noalloc */
caml_iomux_poll_get_fd(value v_fds, value v_index)
{
	pollfd_t *pfd = pollfd_of_index(v_fds, v_index);

	return (Val_int(pfd->fd));
}

/*
 * Util
 */

value
caml_iomux_poll_max_open_files(value v_unit)
{
	CAMLparam1(v_unit);
	long r;

#ifdef IS_WINDOWS
	/* On Windows, use _getmaxstdio() for the default CRT file limit.
	 * Note: This is for file handles, not socket handles. Windows sockets
	 * have different limits. We clamp to a reasonable value. */
	r = _getmaxstdio();
	if (r == -1)
		r = 2048; /* Windows default */
	/* Clamp to reasonable maximum */
	if (r > 524288)
		r = 524288;
#else
	r = sysconf(_SC_OPEN_MAX);
	if (r == -1) /* this allocs */
		uerror("poll_max_open_files", Nothing);
	else if (r > 524288)
		r = 524288;
#endif

	CAMLreturn (Val_int(r));
}
