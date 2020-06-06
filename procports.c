#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <ctype.h>
#include "procports.h"

// FIXME general - several of these functions could be implemented
// by having a connection iterator, parameterised by a handling
// function??

// FIXME only hangs up 1 connection, repeat if needed
// Hangs up a random connection associated with given
// local port.
// -1 on error, sets errstr
int
hanguplocalport(int port)
{
	char path[50];
	int conn, fd;

	conn = localportconn(port);
	if(conn < 0) return 0;
	snprint(path, sizeof(path)-1, "/net/tcp/%d/ctl", conn);
	fd = open(path, OWRITE);
	if(fd < 0) return -1;
	if(write(fd, "hangup", 6) < 0) return -1;
	return 0;
}

// Stores in data the first 100 bytes from /net/tcp/NUM/THING
void
tcpinfo(char data[], int num, char *thing)
{
	int fd, n;
	char path[50];
	int max = 100;
	snprintf(path, sizeof(path)-1, "/net/tcp/%d/%s", num, thing);
	fd = open(path, OREAD);
	if(fd < 0) sysfatal("conninfo(%d) open %s: %r", num, path);
	n = read(fd, data, max);
	close(fd);
	if(n < 0) sysfatal("conninfo(%d) read %s: %r", num, path);
	assert(n <= max);
	data[n-1] = '\0';
}

struct ConnInfo
{
	int n;
	char *status;		/* Listen|Closed|Established ... */
	char *lsys;         /* local system */
	char *lserv;        /* local service */
	char *rsys;         /* remote system */
	char *rserv;        /* remote service */
	char *laddr;        /* local address */
	char *raddr;        /* remote address */
};
typedef struct ConnInfo ConnInfo;

// Free with freeconninfo()
ConnInfo*
conninfo(int num)
{
	char data[100];
	char *p;
	ConnInfo* ci = malloc(sizeof(ConnInfo));
	assert(ci);
	ci->n = num;

	tcpinfo(data, num, "local");
	ci->laddr = strdup(data);
	p = strchr(data, '!');
	*p = '\0';
	ci->lsys = strdup(data);
	ci->lserv = strdup(p+1);

	tcpinfo(data, num, "remote");
	ci->raddr = strdup(data);
	p = strchr(data, '!');
	*p = '\0';
	ci->rsys = strdup(data);
	ci->rserv = strdup(p+1);

	tcpinfo(data, num, "status");
	*strchr(data, ' ') = '\0';
	ci->status = strdup(data);

	return ci;
}

void
freeconninfo(ConnInfo* ci)
{
	if(!ci) return;
	free(ci->lsys);
	free(ci->lserv);
	free(ci->rsys);
	free(ci->rserv);
	free(ci->laddr);
	free(ci->raddr);
	free(ci->status);
}

extern char *linuxhost;
extern char *cbhost;

// Looks under /net/tcp for conns with status Listen or Established.
// returns malloced array of ints terminated with -1.
int*
portsbusy()
{
	Dir *dirs;
	int fd;
	long count;
	int *ports, ports_count = 0;

	fd = open("/net/tcp", OREAD);
	if(fd < 0) sysfatal("portsbusy: open: %r");
	count = dirreadall(fd, &dirs);
	if(count < 0) sysfatal("portsbusy: dirreadall: %r");
	close(fd);
	ports = malloc(count * sizeof(int));
	assert(ports);
	for(int i = 0; i < count; i++){
		ConnInfo* ci;
		int num;
		Dir *dir = &dirs[i];
		char *name = dir->name;
		if(!isdigit(*name)) continue;
		num = atoi(dir->name);
		ci = conninfo(num);
		if(strcmp(ci->status, "Listen") == 0 ||
			strcmp(ci->status, "Established") == 0) {
			ports[ports_count++] = atoi(ci->lserv);
		}
		freeconninfo(ci);
	}
	free(dirs);
	ports = realloc(ports, (ports_count+1)*sizeof(int));
	ports[ports_count] = -1;
	return ports;
}

// FIXME only returns 1 connection
int
localportconn(int port)
{
	Dir *dirs;
	int fd, result = -1;
	long count;
	char port_s[10];

	snprint(port_s, sizeof(port_s), "%d", port);

	fd = open("/net/tcp", OREAD);
	if(fd < 0) sysfatal("localportconn: open: %r");
	count = dirreadall(fd, &dirs);
	if(count < 0) sysfatal("localportconn: dirreadall: %r");
	close(fd);
	for(int i = 0; i < count; i++){
		ConnInfo* ci;
		int num;
		Dir *dir = &dirs[i];
		char *name = dir->name;
		if(!isdigit(*name)) continue;
		num = atoi(dir->name);
		ci = conninfo(num);
		if(strcmp(ci->lserv, port_s) == 0 &&
			(strcmp(ci->status, "Listen") == 0 ||
			strcmp(ci->status, "Established") == 0))
			result = num;
		freeconninfo(ci);
		if(result >= 0) break;
	}
	free(dirs);
	return result;
}

