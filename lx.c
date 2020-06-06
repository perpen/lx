#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <stdio.h>
#include <bio.h>
#include <regexp.h>
#include "procports.h"

#define FDOFF 20
#define CONNTIMEOUTMS 2000

void* emalloc(int n);
int edup(int a, int b);
int esnprint(char *tgt, int max, char *fmt, ...);
void _dbg(int iserror, char *format, ...);
void logsink(void *arg);
void usage(void);
void configlock(int take);
int notehandle(void*, char *note);
char* handlemsg(char *msg);
void lxsend(char *msg);
int getport(void);
void startfs(void);
void stopfs(void);
char* remote(void);
void vncviewer(int dpy);
void parseconf(char *path);
void config(int argc, char **argv);

struct {
	int minport, maxport, fsport, mainfd, cmdlen,
		checkcwd, debug, dbgfd, lckfd;
	char *srvhost, *srvport, *cbhost, *defcmd, *defmounts,
		*cwd, *mounts, *progname, *tmpdir;
	char **cmd;
} g;

struct {
	int pid; // only this thread handles notes
	int count;
	vlong lasttime;
} notectx;

// Writes to log file.
// Will be printed to stderr as well if it's an error msg or
// in debug mode.
void
_dbg(int iserror, char *format, ...)
{
	va_list arg;
	char buf[1000];
	char *prefix = "client: ";
	int prelen = strlen(prefix);
	strcpy(buf, prefix);
	va_start(arg, format);
	vsnprint(buf + prelen, sizeof buf - prelen - 1, format, arg);
	va_end(arg);
	if(g.dbgfd != -1 && g.dbgfd != 2) fprint(g.dbgfd, buf);
	if(g.debug || iserror) fprint(2, buf);
}

#define dbg(...) do{ _dbg(0, __VA_ARGS__); }while(0)
#define error(...) do{ _dbg(1, __VA_ARGS__); }while(0)

void*
emalloc(int n)
{
	void *v = mallocz(n, 1);
	if(v == nil) sysfatal("out of memory allocating %d", n);
	return v;
}

int
edup(int a, int b)
{
	int x = dup(a, b);
	if(x < 0) sysfatal("dup: %r");
	return x;
}

int
esnprint(char *tgt, int max, char *fmt, ...)
{
	va_list arg;
	int n;
	va_start(arg, fmt);
	n = vsnprint(tgt, max, fmt, arg);
	if(n == max) sysfatal("formatted string too long: '%s'", fmt);
	return n;
}

// Makes dir if doesn't exist, aborts on error
void
ensuredir(char *s, int mode)
{
	int f;
	if(access(s, AEXIST) == 0) return;
	f = create(s, OREAD, DMDIR | mode);
	if(f < 0) sysfatal("ensuredir: can't create %s: %r\n", s);
	close(f);
}

void
logsink(void *arg)
{
	int infd = *(int*)arg, outfd;
	Ioproc *io = ioproc();
	char outpath[1000];

	esnprint(outpath, sizeof outpath, "%s/%s.%d.log",
		g.tmpdir, g.srvhost, g.fsport);
	outfd = create(outpath, OWRITE, 0600|DMAPPEND);
	if(outfd < 0) sysfatal("cannot create %s: %r", outpath);

	for(;;){
		char buf[100];
		int n = ioread(io, infd, buf, sizeof(buf)-1);
		if (n < 0){
			fprint(2, "logsink: read: %r\n");
			break;
		}
		if(n == 0) break;
		int w = iowrite(io, outfd, buf, n);
		if(w != n)
			fprint(2, "logsink: wrote %d instead of %d\n", w, n);
	}
	closeioproc(io);
}

// Sends message to server
void
lxsend(char *msg)
{
	int n;
	dbg("lxsend '%s'\n", msg);
	if(g.mainfd < 0) return;
	n = write(g.mainfd, msg, strlen(msg));
	if(n != strlen(msg)) sysfatal("lxsend: unable to send '%s'", msg);
}

// Only notes against the main thread are handled
int
notehandle(void*, char *note)
{
	long ns = 1000000000/3; // third of a second
	vlong t;

	if(getpid() != notectx.pid) return 0;
	if(g.mainfd == -1 && strcmp(note, "alarm") == 0)
		sysfatal("connection timed out");
	if(strcmp(note, "interrupt") != 0) return 1;
	dbg("notehandle: pid=%d note %s\n", getpid(), note);

	t = nsec();
	if(t - notectx.lasttime > ns){
		notectx.count = 0;
		notectx.lasttime = t;
	}
	notectx.count++;
	switch(notectx.count){
	case 1:
		lxsend("int");
		break;
	case 2:
		fprint(2, "sending SIGHUP\n");
		lxsend("hup");
		notectx.lasttime = t;
		break;
	case 3:
		fprint(2, "sending SIGKILL\n");
		lxsend("kill");
		break;
	}
	return 1;
}

void
vncviewer(int dpy)
{
	char vncaddr[50];
	esnprint(vncaddr, sizeof(vncaddr)-1, "%s:%d", g.srvhost, dpy);

	switch(fork()){
	case -1:
		sysfatal("vncviewer: fork: %r");
	case 0:
		dbg("vncviewer: vnc addr=%s\n", vncaddr);
		edup(g.dbgfd, 1);
		edup(g.dbgfd, 2);
		execl("/bin/vncv", "vncv", vncaddr, NULL);
		sysfatal("vncviewer: exec vncv %s: %r", vncaddr);
	}
}

// Handles message from server
// Returns the exit message if the remote process exited, else nil
char*
handlemsg(char *msg)
{
	char buf[200];
	int dpy;

//	dbg("handlemsg: received '%s'\n", msg);
	if(strlen(msg) >= sizeof(buf)-1)
		error("handlemsg: msg longer than %d\n", sizeof buf);

	if(sscanf(msg, "vnc %d", &dpy) == 1)
		vncviewer(dpy);
	else if(sscanf(msg, "exit %s", buf) == 1)
		return strdup((strcmp(msg, "exit 0") == 0) ? "" : msg);
	else if(sscanf(msg, "fatal: %s", buf) == 1){
		fprint(2, "%s: %s\n", g.progname, msg);
		return strdup(msg);
	}else
		fprint(2, "%s: bug: malformed message: %s\n",
			g.progname, msg);
	return nil;
}

// Comms with server
// Returns the remote command exit message
char*
remote(void)
{
	Ioproc *io = ioproc();
	char *addr, *exitmsg = nil;

	addr = netmkaddr(g.srvhost, nil, g.srvport);
	g.mainfd = -1;
	alarm(CONNTIMEOUTMS);
	g.mainfd = dial(addr, nil, nil, nil);
	if(g.mainfd < 0) sysfatal("remote: dial %s: %r", addr);

	// push params and command
	fprint(g.mainfd, "%q\n%d\n%d\n%q\n%d\n%q\n",
		g.cbhost, g.fsport, g.debug, g.cwd, g.checkcwd, g.mounts);
	for(int i = 0; i < g.cmdlen; i++)
		fprint(g.mainfd, "%s\n", g.cmd[i]);
	fprint(g.mainfd, "LXEND\n");

	for(;;){
//		dbg("remote: read loop\n");
		char buf[200];
		int n = ioread(io, g.mainfd, buf, sizeof(buf)-1);
		if(n <= 0){
			// Don't leave the loop if interrupted by a note,
			char err[ERRMAX] = {0};
			errstr(err, sizeof err);
			dbg("remote: read err='%s'\n", err);
			if(*err && strcmp(err, "interrupted")) break;
			continue;
		}
		buf[n] = '\0';
		if(n == sizeof(buf)) sysfatal("message too big: '%s'", buf);
		dbg("remote: received '%s'\n", buf);
		exitmsg = handlemsg(buf);
		if(exitmsg != nil) break;
	}
	dbg("remote: post-read\n");
	close(g.mainfd);
	closeioproc(io);
	return exitmsg;
}

// sysfatal if can't get port
int
getport(void)
{
	int *busyports;
	int port;
	configlock(1);
	busyports = portsbusy();
	for(port = g.minport; port <= g.maxport+1; port++){
		int busy = 0;
		for(int *busyp = busyports; *busyp != -1; busyp++){
			if(*busyp == port){
				busy = 1;
				break;
			}
		}
		if(!busy) break;
	}
	free(busyports);
	configlock(0);

	if(port > g.maxport) sysfatal("no available port");
	return port;
}

// Listen for exportfs connection
void
startfs(void)
{
	char addr[32];
	switch(fork()){
	case -1:
		sysfatal("startfs: fork: %r");
	case 0:
		esnprint(addr, sizeof(addr), "tcp!*!%d", g.fsport);
		dbg("startfs: listen1 on %s\n", addr);
		edup(g.dbgfd, 1);
		edup(g.dbgfd, 2);
		execl("/bin/aux/listen1", "aux/listen1", "-tv1", addr,
			"/bin/exportfs", "-r", "/",
			NULL);
		sysfatal("startfs: exec: %r");
	}
}

void
stopfs(void)
{
	dbg("stopfs\n");
	if(getpid() != notectx.pid) return;
	if(g.fsport >= 0 && hanguplocalport(g.fsport) < 0)
		sysfatal("cannot hangup exportfs conn: %r");
}

// Pass 1 to take, 0 to release
void
configlock(int take)
{
	char path[40];
	esnprint(path, sizeof(path), "%s/lock", g.tmpdir);
	if(take){
		assert(g.lckfd == -1);
		// Try for 10s at least
		for(int i = 0; i < 200; i++){
			g.lckfd = create(path, OREAD, 0600|DMEXCL);
			if(g.lckfd >= 0) break;
			sleep(50);
		}
		if(g.lckfd < 0)
			sysfatal("configlock: timed out waiting %s", path);
	}else{
		assert(g.lckfd != -1);
		close(g.lckfd);
		g.lckfd = -1;
	}
}

// sysfatal on parsing error or missing config entry
void
parseconf(char *path)
{
	char *line;
	Reprog *rx = regcomp("^((([^=]+)(\\.))?([^.=]+))(=)(.*)$");
	assert(rx);

	// First pass for default values, second for host overrides
	for(int pass = 1; pass <= 2; pass++){
		Biobuf* bbuf = Bopen(path, OREAD);
		if(bbuf == nil) sysfatal("config Bopen: %r");
		while(line = Brdline(bbuf, '\n')){
			Resub match[10];
			int len;
			char *host, *name, *val;

			if(line[0] == '#') continue;
			len = Blinelen(bbuf);
			line[len-1] = '\0';

			memset(match, 0, 10*sizeof(Resub));
			if(regexec(rx, line, match, 8) != 1)
				sysfatal("invalid line in %s: %s", path, line);
			if(match[4].sp != nil) *match[4].sp = '\0';
			*match[6].sp = '\0';
			host = match[3].sp;
			name = match[5].sp;
			val = match[7].sp;

			if(pass == 1 && host != nil)
				continue;
			if(pass == 2 &&
				(host == nil || strcmp(host, g.srvhost) != 0))
				continue;

			if(strcmp(name, "default-host") == 0 && g.srvhost == nil)
				g.srvhost = strdup(val);
			else if(strcmp(name, "mounts") == 0)
				g.defmounts = strdup(val);
			else if(strcmp(name, "port") == 0)
				g.srvport = strdup(val);
			else if(strcmp(name, "minport") == 0)
				g.minport = atoi(val);
			else if(strcmp(name, "maxport") == 0)
				g.maxport = atoi(val);
			else if(strcmp(name, "command") == 0)
				g.defcmd = strdup(val);
			else if(strcmp(name, "callback-host") == 0)
				g.cbhost = strdup(val);
		}
		Bterm(bbuf);
	}

	if(g.defmounts == nil) sysfatal("missing mounts config");
	if(g.srvport == 0) sysfatal("missing port config");
	if(g.minport == 0) sysfatal("missing minport config");
	if(g.maxport == 0) sysfatal("missing maxport config");
	if(g.defcmd == nil) sysfatal("missing command config");
//	if(g.cbhost == nil) sysfatal("missing callback-host config");

	free(rx);
}

// Parses command line and $home/lib/lx, populates global vars
void
config(int argc, char **argv)
{
	char *home, *slash;
	char path[100], cwd2[1000];
	g.srvhost = nil;

	ARGBEGIN {
	default:
		usage();
	case 'd':
		g.debug = 1;
		break;
	case 'D':
		g.checkcwd = 1;
		break;
	case 'h':
		g.srvhost = strdup(EARGF(usage()));
		break;
	case 'c':
		g.cwd = strdup(EARGF(usage()));
		break;
	case 'm':
		g.mounts = strdup(EARGF(usage()));
		break;
	} ARGEND

	slash = strrchr(argv0, '/');
	if(slash != nil)
		g.progname = slash + 1;
	else
		g.progname = argv0;

	if(g.cwd == nil){
		if(getwd(cwd2, sizeof(cwd2)-1) == nil)
			sysfatal("config getwd: %r");
		g.cwd = strdup(cwd2);
	}

	home = getenv("home");
	esnprint(path, sizeof(path), "%s/lib/%s", home, g.progname);
	free(home);
	parseconf(path);

	if(g.cbhost == nil){
		char buf[20];
		int n, fd = open("/dev/sysname", OREAD);
		if(fd < 0) sysfatal("cannot open /dev/sysname: %r");
		n = read(fd, buf, sizeof buf - 1);
		if(n < 0)
			sysfatal("cannot read /dev/sysname: %r");
		buf[n] = '\0';
		g.cbhost = strdup(buf);
	}
	if(g.mounts == nil) g.mounts = g.defmounts;

	if(argc > 0){
		g.cmd = malloc(argc * sizeof(char*));
		assert(g.cmd);
		for(int i = 0; i < argc; i++)
			g.cmd[i] = strdup(argv[i]);
		g.cmdlen = argc;
	}else{
		// use default command
		int maxargs = 100;
		g.cmd = malloc((maxargs)*sizeof(char*));
		assert(g.cmd);
		g.cmdlen = tokenize(g.defcmd, g.cmd, maxargs);
		if(g.cmdlen == maxargs)
			sysfatal("more than %d args", maxargs-1);
	}
	assert(g.cmdlen);
}

void
usage(void)
{
	fprint(2, "usage: %s [-d] [-h host] [-c dir] [-D] [-m mounts] cmd ...\n",
		argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *exitmsg;
	char tmp[100];
	int in = FDOFF, out = in+1, err = in+2;
	int logpipe[2];

	if(rfork(RFFDG|RFNAMEG) < 0) sysfatal("threadmain: rfork: %r");

	quotefmtinstall();

	g.mainfd = -1;
	g.lckfd = -1;
	g.dbgfd = 2; // until log file is setup, log to stderr

	assert(dup(0, in) == in);
	assert(dup(1, out) == out);
	assert(dup(2, err) == err);

	config(argc, argv);

	esnprint(tmp, sizeof tmp, "/root/tmp/%s.%s",
		g.progname, getuser());
	g.tmpdir = tmp;
	ensuredir(g.tmpdir, 0700);

	g.fsport = getport();

	pipe(logpipe);
	g.dbgfd = logpipe[0];
	proccreate(logsink, &logpipe[1], 8*1024);

	notectx.pid = getpid();
	notectx.lasttime = 0;
	startfs();
	atexit(stopfs);
	assert(atnotify(notehandle, 1) > 0);
	exitmsg = remote();
	threadexitsall(exitmsg);
}
