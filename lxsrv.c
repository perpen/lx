// for unshare(2)
#define _GNU_SOURCE

#include <u.h>
// unix start
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
extern int mkdir(const char *pathname, mode_t mode);
// unix end
#include <libc.h>
#include <thread.h>

#define DEFAULTPORT "9000"
#define NINEMNT "/9"
#define FDOFF 20
#define VNCMAXCOUNT 100
#define VNCMINDPY 100
// Increase this if "can't open display" errors
// FIXME make into a config param? plus can be machine-dependent
#define VNCWAITMS 200

char *progname, *plan9dir, *tmpdir, *mntbasedir;
int lckfd;

// Using this struct simply for grouping session-related globals
struct {
	char *host, *pxydir, *tmp;
	int port, logfd, debug, fd9, pid, clientended;
	int pxyfd, pxydpy, vncdpy, x11count;
	QLock x11lck;
} S;

typedef struct {
	char *host, *cwd, *mounts;
	int port;
	char **argv;
	int debug, checkcwd;
} Params;

void session();
void usage();
void _dbg(int iserror, char *format, ...);
void* emalloc(int n);
void* erealloc(void *v, int n);
void* estrdup(void *v);
static long _iolisten(va_list *arg);
int iolisten(Ioproc *io, char *a, char *b);
static long _ioaccept(va_list *arg);
int ioaccept(Ioproc *io, int fd, char *dir);
static long _ioselect(va_list *arg);
int ioselect(Ioproc *io, int maxfd, fd_set *fds,
	struct timeval *timeout);
int esnprint(char *tgt, int max, char *fmt, ...);
void mkdir_p(char *path, mode_t mode);
void exit9(int unexpected, char *exitmsg);
void sysfatal9(char *fmt, ...);
int send9(char *msg);
void logsink(void *arg);
void getwinsize(char *out, int len);
void killvnc();
int waitforvnc();
int proxychunk(Ioproc *io, int src, int tgt);
void proxy(void *arg);
void x11conn(void *arg);
void x11listen(void *arg);
void command(char *cwd, char **argv);
Params getparams();
char* readparamsblock(int fd);
void p9prun(char *a0, char *a1, char *a2);
void setupns(char *host, int port, char *mounts);
void setupio();
void handlenote(char *note);
void control(void *arg);
void cleanup();
int createpxysock();

#define dbg(...) do{ _dbg(0, __VA_ARGS__); }while(0)
#define error(...) do{ _dbg(1, __VA_ARGS__); }while(0)

void
_dbg(int iserror, char *format, ...)
{
	va_list arg;
	char buf[1000];
	char *prefix = "server: ";
	int prelen = strlen(prefix);
	strcpy(buf, prefix);
	va_start(arg, format);
	vsnprint(buf + prelen, sizeof(buf) - prelen - 1, format, arg);
	va_end(arg);
	if(S.debug || iserror)
		// client must show this
		fprint(2, buf);
	if(S.logfd != 0)
		// offset b/c no need for prefix in log file
		fprint(S.logfd, buf+prelen);
}

void*
emalloc(int n)
{
	void *v = mallocz(n, 1);
	if(v == nil) sysfatal("out of memory allocating %d", n);
	return v;
}

void*
erealloc(void *v, int n)
{
	v = realloc(v, n);
	if(v == nil) sysfatal("out of memory reallocating %d", n);
	return v;
}

void*
estrdup(void *v)
{
	v = strdup(v);
	if(v == nil) sysfatal("out of memory strdup'ing %s", v);
	return v;
}

static long
_iolisten(va_list *arg)
{
	char *a, *b;
	a = va_arg(*arg, char*);
	b = va_arg(*arg, char*);
	return listen(a, b);
}

int
iolisten(Ioproc *io, char *a, char *b)
{
	return iocall(io, _iolisten, a, b);
}

static long
_ioaccept(va_list *arg)
{
	int fd;
	char *dir;
	fd = va_arg(*arg, int);
	dir = va_arg(*arg, char*);
	return accept(fd, dir);
}

int
ioaccept(Ioproc *io, int fd, char *dir)
{
	return iocall(io, _ioaccept, fd, dir);
}

static long
_ioselect(va_list *arg)
{
	int maxfd = va_arg(*arg, int);
	fd_set *fds = va_arg(*arg, fd_set *);
	struct timeval *timeout = va_arg(*arg, struct timeval *);
	return select(maxfd, fds, NULL, NULL, timeout);
}

int
ioselect(Ioproc *io, int maxfd, fd_set *fds, struct timeval *timeout)
{
	return iocall(io, _ioselect, maxfd, fds, timeout);
}

int
esnprint(char *tgt, int max, char *fmt, ...)
{
	va_list arg;
	int n;
	va_start(arg, fmt);
	n = vsnprint(tgt, max, fmt, arg);
	va_end(arg);
	if(n == max)
		sysfatal9("formatted string too long: '%s'", fmt);
	return n;
}

void
session()
{
	Params params = getparams();

	S.debug = params.debug;
	S.host = params.host;
	S.port = params.port;

	int logpipe[2];
	int *logfdin = emalloc(sizeof(int));
	pipe(logpipe);
	S.logfd = logpipe[0];
	*logfdin = logpipe[1];
	threadcreate(logsink, logfdin, 32*1024);

	if(createpxysock() < 0)
		sysfatal9("unable to find a free vnc proxy port");
	S.vncdpy = S.pxydpy + VNCMAXCOUNT;

	atexit(cleanup);
	setupns(params.host, params.port, params.mounts);
	setupio();
	threadcreate(control, nil, 32*1024);
	threadcreate(x11listen, nil, 32*1024);
	if(chdir(params.cwd) < 0)
		if(params.checkcwd){
			char msg[100];
			snprintf(msg, sizeof msg, "cannot cd to %s", params.cwd);
			exit9(1, msg);
			return;
		}else
			fprint(2, "warning: cannot cd to %s\n", params.cwd);
	command(params.cwd, params.argv);
	dbg("session: done\n");
}

// Tell client to exit with given message, truncated if too long.
// If unexpected, we prefix the message with "fatal:", which tells
// the client to print it on stderr before exiting.
void
exit9(int unexpected, char *exitmsg)
{
	char buf[300];
	esnprint(buf, sizeof(buf), "%s %s",
		unexpected ? "fatal:" : "exit",
		exitmsg);
	send9(buf);
	S.clientended = 1;
}

// Like sysfatal, but notifies the client before aborting
void
sysfatal9(char *fmt, ...)
{
	char buf[300];
	va_list arg;
	int n;
	va_start(arg, fmt);
	n = vsnprint(buf, sizeof(buf), fmt, arg);
	va_end(arg);
	if(n == sizeof(buf))
		error("sysfatal9: formatted string too long: '%s'", fmt);
	if(S.fd9 >= 0) exit9(1, buf);
	sysfatal(buf);
}

void
mkdir_p(char *path, mode_t mode)
{
	char *slash = path;
	if(strlen(path) == 0) sysfatal9("mkdir_p empty path");
	for(;;){
		slash = strchr(slash+1, '/');
		if(slash != nil) *slash = '\0';
		if(mkdir(path, mode) < 0 && errno !=EEXIST)
			sysfatal9("mkdir_p: mkdir %s: %r", path);
		if(slash == nil) break;
		*slash = '/';
	}
}

// Takes/release a lockfile unique to our progname and user
// Pass 1 to take, 0 to release
// Using a lock file so it works with multiple server instances
void
configlock(int take)
{
	char path[40];
	esnprint(path, sizeof(path), "%s/lock", tmpdir);
	if(take){
		// Try for 10s at least
		for(int i = 0; i < 200; i++){
			lckfd = create(path, OWRITE|OLOCK, 0600);
			if(lckfd >= 0) break;
			sleep(50);
		}
		if(lckfd < 0)
			sysfatal("configlock: timed out waiting %s", path);
	}else{
		close(lckfd);
	}
}

// Looks in /tmp/.X11-unix/ for an avalaible socket in our range,
// creates a socket, updates S.pxy* globals
// Returns -1 on error.
int
createpxysock()
{
	char sockpath[100];
	struct stat st;
	int afd, dpy = -1;

	configlock(1);

	for(int i = VNCMINDPY; i <= VNCMINDPY + VNCMAXCOUNT - 1; i++){
		esnprint(sockpath, sizeof(sockpath),
			"/tmp/.X11-unix/X%d", i);
		if(stat(sockpath, &st) < 0){
			dpy = i;
			break;
		}
	}
	if(dpy < 0) return -1;

	char adir[40];
	char pxyaddr[40];
	esnprint(pxyaddr, sizeof(pxyaddr),
		"unix!/tmp/.X11-unix/X%d", dpy);
	afd = announce(pxyaddr, adir);
	dbg("createpxysock: announce %s - %s\n", pxyaddr, adir);

	configlock(0);

	S.pxyfd = afd;
	S.pxydpy = dpy;
	S.pxydir = strdup(adir);
	return 0;
}

// Reads from the log fd (passed as param) into the log file
void
logsink(void *arg)
{
	char outpath[1000];
	int outfd;
	int fd = *(int*)arg;
	Ioproc *io = ioproc();

	esnprint(outpath, sizeof outpath, "%s/%s.%d.log",
		tmpdir, S.host, S.port);
	outfd = create(outpath, OWRITE|OAPPEND, 0600);
	if(outfd < 0) sysfatal9("cannot create %s: %r", outpath);

	for(;;){
		char buf[100];
		int n = ioread(io, fd, buf, sizeof(buf)-1);
		if(n <= 0) break;
		int w = iowrite(io, outfd, buf, n);
		if(w != n)
			fprint(2, "logsink: wrote %d instead of %d\n", w, n);
	}
}

// Messages the client on 9, -1 on error
int
send9(char *msg)
{
	int status = 0;
	dbg("send9: '%s'\n", msg);
	Ioproc *io = ioproc();
	int w = iowrite(io, S.fd9, msg, strlen(msg)+1);
	if(w < strlen(msg)+1){
		error("send9: only wrote %d/%d !?\n", w, strlen(msg)+1);
		status = -1;
	}
	closeioproc(io);
	return status;
}

// Queries the window system on 9
void
getwinsize(char *out, int len)
{
	int rioborder = 4;
	char wctl[30];
	esnprint(wctl, sizeof(wctl), "%s/mnt/wsys/wctl", NINEMNT);
	FILE *f = fopen(wctl, "r");
	if(f == nil) sysfatal9("getwinsize: fopen %s: %r", wctl);
	int x1, y1, x2, y2;
	assert(fscanf(f, "%d %d %d %d ", &x1, &y1, &x2, &y2) == 4);
	esnprint(out, len, "%dx%d", x2-x1-2*rioborder, y2-y1-2*rioborder);
	fclose(f);
}

// We can't just kill our vncserver process, Xvnc is a child of 1
// So we use pkill.
void
killvnc()
{
	dbg("killvnc\n");
	char rx[50];
	esnprint(rx, sizeof(rx), "Xvnc :%d ", S.vncdpy);
	int fds[] = { dup(0, -1), dup(S.logfd, -1), dup(S.logfd, -1) };
	if(threadspawnl(fds, "/bin/pkill", "pkill", "-f", rx, NULL) < 0)
		error("killvnc: cannot start pkill: %r\n");
}

// Waits for the vnc socket to appear, returns -1 on timeout
int
waitforvnc()
{
	Ioproc *io = ioproc();
	char vncsock[30];
	esnprint(vncsock, sizeof(vncsock), "/tmp/.X11-unix/X%d", S.vncdpy);
	int result = -1;
	for(int i = 0; i < 50; i++){
		struct stat st;
		if(stat(vncsock, &st) >= 0) {
			result = 0;
			break;
		}
		iosleep(io, 100);
	}
	if(1){
		// FIXME fragile machine-specific value, should poll instead
		iosleep(io, VNCWAITMS);
	}else{
		// FIXME not working
		char vncaddr[40];
		esnprint(vncaddr, sizeof(vncaddr), "unix!%s", vncsock);
		int vncfd = iodial(io, vncaddr, 0, 0, 0);
		if(vncfd < 0) sysfatal9("LOOP: %r");
		for(;;){
			dbg("LOOP\n");
			int w = iowrite(io, vncfd, "x", 1);
			dbg("LOOP w=%d\n", w);
			if(w == 1){
				char buf[1];
				ioread(io, vncfd, buf, 1);
				break;
			}
			perror("x");
			iosleep(io, 10);
		}
		ioclose(io, vncfd);
	}
	closeioproc(io);
	return result;
}

// Forwards a readfull from src to tgt, returns -1 on eof or error
int
proxychunk(Ioproc *io, int src, int tgt)
{
	char buf[8192];
	int n = ioread(io, src, buf, sizeof(buf));
	if(n < 0){
		dbg("proxychunk: read: %r\n");
		return -1;
	}
	if(n == 0){
//		dbg("proxychunk: eof\n");
		return -1;
	}
	int nw = iowrite(io, tgt, buf, n);
	if(nw != n) {
		error("proxychunk: write: %r\n");
		return -1;
	}
	return 0;
}

typedef struct {
	int pxyfd, vncfd;
	Channel *chan;
} ProxyContext;

// Thread proxying from proxy to vnc sockets
void
proxy(void *arg)
{
	ProxyContext *ctx = arg;
	Ioproc *io = ioproc();
	fd_set rfds;
	struct timeval tv;
	int retval;
 	int maxfd = (ctx->pxyfd > ctx->vncfd) ? ctx->pxyfd : ctx->vncfd;

	for(;;){
		FD_ZERO(&rfds);
		FD_SET(ctx->pxyfd, &rfds);
		FD_SET(ctx->vncfd, &rfds);
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		retval = ioselect(io, maxfd+1, &rfds, &tv);
		if(retval < 0){
			error("proxy: select %d %d: %r\n", ctx->pxyfd, ctx->vncfd);
			break; // FIXME or continue?
		}
		if(retval == 0) continue; // select timeout
		if(FD_ISSET(ctx->pxyfd, &rfds))
			if(proxychunk(io, ctx->pxyfd, ctx->vncfd) < 0) break;
		if(FD_ISSET(ctx->vncfd, &rfds))
			if(proxychunk(io, ctx->vncfd, ctx->pxyfd) < 0) break;
	}
	ioclose(io, ctx->pxyfd);
	ioclose(io, ctx->vncfd);
	closeioproc(io);
	sendul(ctx->chan, 0);
}

typedef struct {
	int lfd;
	char ldir[40];
} X11connContext;

// Handles a connection to the proxy socket:
// - starts vncserver if first connection
// - proxies to the vnc socket
// - asks the 9 client to start the vnc viewer
// - on closing the last connection, stops viewer and server
void
x11conn(void *arg)
{
	X11connContext *ctx = arg;
	Ioproc *io = ioproc();

	int cfd = ioaccept(io, ctx->lfd, ctx->ldir);
	qlock(&S.x11lck); // start vnc server once
	S.x11count++;
	if(cfd < 0){
		error("x11conn: ioaccept %r\n");
		qunlock(&S.x11lck);
		goto out;
	}

//	dbg("x11conn: lfd=%d x11count=%d\n", ctx->lfd, S.x11count);
	int startviewer = 0;
	if(S.x11count == 1){
		startviewer = 1;
		char colondpy[10], winsize[20];
		getwinsize(winsize, sizeof(winsize));
		esnprint(colondpy, sizeof(colondpy), ":%d", S.vncdpy);
		int fds[] = { dup(0, -1), dup(S.logfd, -1), dup(S.logfd, -1) };
		dbg("x11conn: spawning vncserver %s %s\n",
			colondpy, winsize);
		if(threadspawnl(fds,
			"/usr/bin/vncserver", "vncserver",
			colondpy, "-fg", "-autokill", "-geometry", winsize,
			NULL) < 0)
			sysfatal9("x11conn: error starting vncserver: %r");
	}

	int novnc = (waitforvnc() == -1);
	qunlock(&S.x11lck);
	if(novnc){
		error("x11conn: vnc socket not appearing\n");
		// maybe we didn't wait enough, don't leave a vnc hanging
		killvnc();
		goto out;
	}

	if(startviewer){
		char msg[20];
		esnprint(msg, sizeof(msg), "vnc %d", S.vncdpy);
		send9(msg);
	}

	char vncaddr[30];
	esnprint(vncaddr, sizeof(vncaddr), "unix!/tmp/.X11-unix/X%d",
		S.vncdpy);
	int vncfd = iodial(io, vncaddr, 0, 0, 0);
	if(vncfd < 0) sysfatal9("x11conn: vnc dial %r");

	Channel *donechan = chancreate(sizeof(int), 0);
	ProxyContext proxyctx = {
		.chan = donechan,
		.pxyfd = cfd,
		.vncfd = vncfd,
	};
	threadcreate(proxy, &proxyctx, 16*1024);

	recvul(donechan);
	chanfree(donechan);
	ioclose(io, cfd);
out:
	S.x11count--;
	dbg("x11conn: closing x11count=%d\n", S.x11count);
	if(S.x11count == 0) killvnc();
	ioclose(io, ctx->lfd);
	closeioproc(io);
	free(ctx);
}

// Listens on the vnc proxy socket, delegates to x11conn()
void
x11listen(void *arg)
{
	Ioproc *io = ioproc();
	dbg("x11listen: pxydpy=%d vncdpy=%d\n", S.pxydpy, S.vncdpy);

	for(;;){
		X11connContext *x11ctx = emalloc(sizeof(X11connContext));
		x11ctx->lfd = iolisten(io, S.pxydir, x11ctx->ldir);
		if(x11ctx->lfd < 0){
			error("x11listen: iolisten %r\n");
			break;
		}
		threadcreate(x11conn, x11ctx, 8192);
	}
	closeioproc(io);
}

// Runs the Linux command, on exit sends the status to the 9 client
void
command(char *cwd, char **argv)
{
	int unexpected = 1;
	char displayenv[10];
	esnprint(displayenv, sizeof(displayenv), ":%d", S.pxydpy);
	putenv("DISPLAY", displayenv);

	int fds[] = { dup(0, -1), dup(1, -1), dup(2, -1) };
	char msg[200];
	S.pid = threadspawnd(fds, argv[0], argv, cwd);
	if(S.pid < 0){
		dbg("command: threadspawnd: %r");
		esnprint(msg, sizeof(msg), "cannot start %s", argv[0]);
	}else{
		Channel *chan = threadwaitchan();
		Waitmsg *w;
		for(;;){
			w = recvp(chan);
			if(w == nil) sysfatal9("command: recvp: %r");
			if(w->pid == S.pid){
				dbg("command: process exit '%s'\n", w->msg);
				strncpy(msg, *w->msg ? w->msg : "0", sizeof(msg));
				unexpected = 0;
				free(w);
				break;
			}
			free(w);
		}
	}
	exit9(unexpected, msg);
}

// Return's malloced buffer containing what's read from fd until
// a terminating "LXEND\n"
// Errors if input too large.
// Returns nil on error, sets errstr.
char*
readparamsblock(int fd)
{
	int blksz = 1000, usedsz = 0, maxsz = 20000;
	char *buf = emalloc(blksz);
	char *cur = buf;
	char *err = nil;

	for(;;){
		int n = read(fd, cur, blksz-1);
		if(n < 0){
			err = "readparamsblock: read: %r";
			break;
		}
		if(n == 0){
			err = "readparamsblock: read unexpected eof";
			break;
		}
		cur[n] = '\0';
		if(strstr(buf, "LXEND\n") != nil) break;
		usedsz += n;
		cur = buf + usedsz;
		if(usedsz > maxsz){
			err = "readparamsblock: params too large";
			break;
		}
		if(cur+blksz > buf+usedsz){
			buf = erealloc(buf, usedsz+blksz);
			cur = buf+usedsz;
		}
	}

	if(err){
		werrstr(err);
		free(buf);
		buf = nil;
	}
	return buf;
}

Params
getparams()
{
	char *input = readparamsblock(S.fd9); // leak, nm
	if(input == nil){
		char msg[ERRMAX];
		rerrstr(msg, sizeof(msg));
		write(S.fd9, msg, strlen(msg));
		sysfatal9("getparams: %s", msg);
	}
	dbg("getparams: params:%s\n", input);
	char *lines[1000];
	int linescount = gettokens(input, lines, 1000, "\n");
	lines[linescount-1] = nil; // replace LXEND with nil
	if(linescount < 7){
		char *msg = "not enough params";
		write(S.fd9, msg, strlen(msg));
		sysfatal9("getparams: %s", msg);
	}

	Params params = {
		.host = lines[0],
		.port = atoi(lines[1]),
		.debug = atoi(lines[2]),
		.cwd = lines[3],
		.checkcwd = atoi(lines[4]),
		.mounts = lines[5],
		.argv = lines+6,
	};
	return params;
}

// Mounts plan 9 fs on /9, then does bind mounts
void
setupns(char *host, int port, char *mounts)
{
	if(strchr(mounts, ' ') != nil)
		sysfatal9("spaces disallowed in mounts: %s", mounts);

	// New mount namespace
	int status = unshare(CLONE_NEWNS);
	if(status < 0) sysfatal9("setupns: unshare: %r");

	// Required for the mounts to work after unshare call,
	// on archlinux at least
	if(mount("none", "/", NULL, MS_REC|MS_PRIVATE, NULL) < 0)
		sysfatal9("setupns: mount /: %r");

	// Mounts the plan9 fs on /9 by running p9p commands srv and 9pfuse
	{
		char addr[64];
		char srvname[50];
		char srvpath[100];
		char p9pns[100];
		dbg("fuse: file server %s:%d\n", host, port);

		esnprint(p9pns, sizeof p9pns, "%s/ns", tmpdir);
		mkdir_p(p9pns, 0700);
		putenv("NAMESPACE", p9pns);

		esnprint(srvname, sizeof(srvname), "%s_%d", host, port);
		esnprint(srvpath, sizeof(srvpath), "%s/%s", p9pns, srvname);

		esnprint(addr, sizeof(addr), "tcp!%s!%d", host, port);
		if(unlink(srvpath) < 0 && errno != ENOENT)
			sysfatal9("fuse: unlink %s: %r", srvpath);

		dbg("fuse: srv %s %s\n", addr, srvname);
		p9prun("srv", addr, srvname);
		dbg("fuse: 9pfuse %s %s\n", srvpath, NINEMNT);
		p9prun("9pfuse", srvpath, NINEMNT);
	}

	// Do the requested bind-mounts from /9/X to /X
	// "/A:/a,/B:/b"
	char *entries[10];
	int n = getfields(mounts, entries, 10, 1, ",");
	for(int i = 0; i < n; i++){
		char *dirs[2];
		char *entry = entries[i]; // "/A:/a"
		int n2 = getfields(entry, dirs, 2, 0, ":");
		if(n2 != 2) sysfatal9("malformed mount entry: %s", entry);
		char src[200];
		esnprint(src, sizeof(src), "%s%s", NINEMNT, dirs[0]);
		char *tgt = dirs[1];
		dbg("setupns: mount %s %s\n", src, tgt);
		if(mount(src, tgt, "ext4", MS_BIND|MS_PRIVATE, NULL) < 0)
			sysfatal9("setupns: mount: %r");
	}
}

// Runs p9p command, sysfatal on error
void
p9prun(char *a0, char *a1, char *a2)
{
	char bin[200];
	esnprint(bin, sizeof bin, "%s/bin/%s", plan9dir, a0);
	Channel *chan = threadwaitchan();
	int fds[] = { dup(0, -1), dup(S.logfd, -1), dup(S.logfd, -1) };
	int srvpid = threadspawnl(fds, bin, a0, a1, a2, NULL);
	if(srvpid < 0)
		sysfatal9("p9prun: cannot run %s: %r", bin);
	Waitmsg *w = recvp(chan);
	if(w == nil) sysfatal9("p9prun: %s recvp: %r", a0);
	if(strlen(w->msg) > 0)
		sysfatal9("p9prun: %s failed with status %s", a0, w->msg);
	free(w);
}

// Connects 0, 1, 2 to /9/fd/20,21,22
void
setupio()
{
	mode_t modes[] = {
		// modes to use for stdin/out/err
		OREAD,
		OWRITE|OAPPEND,
		OWRITE|OAPPEND,
	};
	for(int fd = 0; fd < 3; fd++){
		char path[40];
		esnprint(path, sizeof(path), "%s/fd/%d", NINEMNT, FDOFF+fd);
		int newfd = open(path, modes[fd]);
		if(newfd < 0) sysfatal9("setupio: open %s: %r", path);
		if(dup(newfd, fd) < 0) sysfatal9("setupio: dup: %r");
		close(newfd);
	}
}

// Handles a note forwarded by the 9 client
void
handlenote(char *note)
{
	int sig = 0;
	dbg("handlenote: '%s'\n", note);
	if(strcmp("int", note) == 0)
		sig = SIGINT;
	else if(strcmp("hup", note) == 0)
		sig = SIGHUP;
	else if(strcmp("kill", note) == 0)
		sig = SIGKILL;
	else
		error("handlenote: unknown note '%s'\n", note);
	if(sig != 0){
		dbg("handlenote: kill leaderpid=%d sig=%d\n", S.pid, sig);
		kill(-S.pid, sig);
	}
}

// Handles messages from the 9 client
void
control(void *arg)
{
	char buf[100];
	Ioproc *io = ioproc();
	for(;;){
//		dbg("control: loop\n");
		int n = ioread(io, S.fd9, buf, sizeof(buf));
		if(n < 0) sysfatal9("control: read: %r");
		if(n == sizeof buf) sysfatal9("control: message too big");
		if(n == 0){
			dbg("control: eof\n");
			break;
		}
		buf[n] = '\0';
		dbg("control: received '%s'\n", buf);
		handlenote(buf);
	}
	closeioproc(io);
}

// Exit handler during request handling, may be invoked on normal
// command exit, or on unexpected error (eg sysfatal).
void
cleanup()
{
	dbg("cleanup\n");

	killvnc();
	char pxysock[40];
	esnprint(pxysock, sizeof(pxysock), "/tmp/.X11-unix/X%d", S.pxydpy);
	if(unlink(pxysock) < 0 && errno != ENOENT)
		error("cleanup: unlink %s: %r\n", pxysock);

	// Paranoia
	if(!S.clientended)
		send9("exit unexpected error, check lx server logs");

	dbg("cleanup: hasten 9pfuse death\n");
	// We already told the client to exit, it should then be
	// stopping the fs on 9. To help 9pfuse realise the
	// remote server is dead and terminate, we access a random
	// path under the mount point.
	char path[40];
	esnprint(path, sizeof(path), "%s/rc", NINEMNT);
	for(;;){
		int fd = open(path, OREAD);
		close(fd);
		if(fd < 0) break;
		sleep(50);
	}
}

void
usage()
{
	fprint(2, "usage: %s [ -i interface ] [ -p port ]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *interface = "localhost", *port = DEFAULTPORT;

	S.fd9 = -1;

	ARGBEGIN {
	default:
		usage();
	case 'i':
		interface = strdup(EARGF(usage()));
		break;
	case 'p':
		port = EARGF(usage());
		if(atoi(port) == 0)
			sysfatal("invalid port: %s", port);
		break;
	} ARGEND

	char *slash = strrchr(argv0, '/');
	if(slash == nil)
		progname = argv0;
	else
		progname = slash + 1;

	plan9dir = getenv("PLAN9");
	if(plan9dir == nil || strlen(plan9dir) == 0)
		sysfatal("PLAN9 env var not defined");

	char tmp[100];
	esnprint(tmp, sizeof tmp, "/tmp/%s.%s", progname, getuser());
	tmpdir = tmp;
	mkdir_p(tmpdir, 0700);

	char adir[40], ldir[40], addr[50];
	esnprint(addr, sizeof(addr), "tcp!%s!%s", interface, port);
	dbg("threadmain: listening on %s\n", addr);
	int acfd = announce(addr, adir);
	if(acfd < 0) sysfatal("threadmain: announce %s: %r", addr);
	for(;;){
		int lcfd = listen(adir, ldir);
		if(lcfd < 0) sysfatal("threadmain: listen on %s: %r", addr);
		pid_t pid = rfork(RFFDG|RFPROC|RFNOWAIT|RFNOTEG);
		switch(pid){
		case -1:
			sysfatal("threadmain: fork: %r");
		case 0:
			S.fd9 = accept(lcfd, ldir);
			if(S.fd9 < 0) sysfatal("threadmain: accept: %r");
			dbg("threadmain: S.fd9=%d\n", S.fd9);
			session();
			close(S.fd9);
			threadexitsall(0);
		default:
			dbg("threadmain: forked to %d\n", pid);
			close(lcfd);
		 }
	}
}
