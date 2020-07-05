lx - running Linux commands from Plan 9
=======================================

`lx` is an attempt at using Linux machines as cpu servers,
providing a user experience analogous to using the Plan 9
command `cpu(1)`:
- The `lx` stdin/out/err are connected to the remote process, and
  notes received by `lx` are translated into signals to the
  Linux process
- The specific namespace of the Plan 9 client is available
  to the Linux process under `/9`; with configuration it is
  possible to have some directories available from Linux under
  the same path as on Plan 9 (e.g. `$home`).
  When possible the working directory is preserved when running
  the remote process
- If `lx` is run from a rio window and an X11 client is started,
 it will be displayed in this window

The intent is to leverage the larger range of software available
on Linux without compromising too much the native Plan 9 user
experience.

No authentication or encryption is implemented, as this is not
necessary for my current use cases.

I run Linux commands on the host running my Plan 9 kvm, but
it should be possible to use a `vmx` VM instead.

Use cases / examples
--------------------

- `rc` shell: `lx`
- Use Linux commands: `ls -l | lx gawk '{print $4}'`
- Browser: `lx firefox`. For integration with plumber, see
  `bin/firefox`
- Write Linux programs from the comfort of Plan 9
- Write Linux programs accessing e.g. `/dev/draw` or `/dev/net`.
  Then in a way `lx` can be seen as implementing a new objtype:
  uglier, slower and limited, but at least giving us access to
  more programming languages than before.

Mechanism
---------

To take a concrete example, here is an approximation of what
happens when I run `lx gcc blah.c` from Plan 9 directory
`/usr/henri/src/blah`:
- The current namespace is exported by calling `exportfs -r /`
- `lx` connects to server `lxsrv` running on Linux, which
  creates a detached process to handle the session.
- This process creates a new mount namespace using `unshare(2)`
  and mounts the fs exported from Plan 9 on `/9`
- It then bind-mounts `/9/usr/henri` (my home directory) onto
  `/usr/henri`
- It chdirs to the working directory of `lx`, i.e.
  `/usr/henri/src/blah`
- It runs `gcc blah.c`, with its stdin/out/err connected to the
  corresponding file descriptors of the `lx` process.
- Pressing the delete key once (resp. twice, thrice) sends SIGINT
  (resp. SIGHUP, SIGKILL) to the Linux process (actually its
  process group).

X11 clients are supported using a VNC server. Each
time `lx` is invoked, a "fake" X11 socket is created.
The Linux command is started with the DISPLAY
environment variable pointing to this socket. If it gets
opened, a VNC server is started on Linux, `vncv` is started
on Plan 9, and traffic is proxied from the proxy
socket to the actual VNC server socket.

Requirements
------------

- On Linux: Plan 9 from User Space
- On Linux: A VNC server if X11 clients will be run
- Connectivity: the client `lx` needs to connect to the port served
  by the Linux server `lxsrv`; The Linux host needs to connect
  to a range of ports connected to `exportfs` servers.

Build
-----

#### Bootstrapping build

- Define the `PLAN9` environment variable pointing to the p9p
  base directory, add `$PLAN9/bin` to your `PATH`.
- Clone the repo on both Plan 9 and Linux
- On Plan 9: `mk -f mkfile.clt install`
- On Linux: `mk -f mkfile.srv install`

#### Further builds

Once you have a working setup and assuming you cloned the repo
in a bind-mounted directory (via the `mounts` config setting
or `lx` option `-m`), you can build client and server
by running `mk both.install` from Plan 9.
This will use `lx` for the linux-side build.

Linux Setup
-----------

Skip the VNC sections below if you will not be running X11 clients.

#### Prepare the mount points
```
$ sudo mkdir /9 && sudo chown $USER:$USER /9
# Optionally, if you want a bind mount of /9/usr/USER on /usr/USER
# This assumes your username is the same on both sides
$ sudo mkdir /usr/$USER && sudo chown $USER:$USER /usr/$USER
```

#### Install Plan 9 from User Space
The namespace exported from Plan 9 is mounted on Linux
using the `9pfuse` command provided by p9p.
Environment variable `PLAN9` must be defined when starting
`lxsrv`, pointing to the p9p base directory.

P9p is required as well to build the server executable, simply
because I decided to use the Plan 9-style p9p libraries for
implementing the server.

#### Running rc on Linux
For commands `"` and `""` to work, you can add at the beginning
of `$PLAN9/bin/wintext` the following line:
```
[ -f /9/dev/text ] && exec cat /9/dev/text
```

#### VNC server
`lx` has only been tested with `tigervnc`, but other implementations
are expected to work with little or no code change.

Once you have confirmed X11 clients work with `lx`, you can
eliminate an overlong hardcoded wait for the VNC server:
In the script `/usr/bin/tigervnc` I changed line
```
sleep(3);
```
into
```
use Time::HiRes qw(usleep); usleep(200000);
```
If this sleep is too short, you will get an error like
`/tmp/.X11-unix/X212: No such file or directory`

#### VNC startup file
Create/edit `~/.vnc/xstartup` with this content:
```
#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
xsetroot -solid grey
xhost +local:
lx-dwm # or any other window manager
```

#### Window manager
The VNC startup file must run a window manager. Any such
program can be used, but a version of `dwm` is provided
here, which hides unnecessary tag buttons and
provides another to close the currently selected
client. To allow coexistence with an existing `dwm`
it is installed as `lx-dwm`.

It will be installed along with `lxsrv` when running the `mk`
command mentioned in the Build section above.

Plan 9 setup
------------

`lx` reads its configuration from path `$home/lib/lx`.

Here is an example allowing connection to a single Linux host
called `rabbit`:
```
default-host=rabbit
minport=3000
maxport=3049
mounts=/usr/roger:/usr/roger
command=rc -il
```
If more customisation is required, refer to man page `lx(6)`.

Bugs
----

`lx` has only been tested in one environment and with a few
commands. These are the known problems:
- Regression: Firefox errors after ~30mn of intensive use (bad file
  descriptor) then needs to be restarted. A workaround I refuse to
  use it to start it from dwm.
- If the `lx` client is slayed, a few processes are leaked
- If an X11 client launched directly from `lx` prints to
  stdout/stderr (e.g. `xev`), the messages will corrupt the
  `vncv` display. Redirect the output if this is a problem.
- The p9p graphical programs do not work, because they do not work
  with VNC server (try others?)
- Some X11 clients (only `llpp` so far) insist on connecting to
  the X server using an abstract socket. But our proxy socket
  is created using p9p's `announce`, which does not support
  abstract. To fix this we'd need to use the linux `socket`
  syscall directly.
- Passing environment variables to a Linux command is currently
  difficult and requires clunky command lines like
  `lx bash -c 'X=1; echo $X'`.
- ~.6s lag on each `lx` invocation, due to running `exportfs`
  and `9pfuse` each time:
  If that ever became a problem for some use case
  it should be possible to maintain a persistent session, e.g.
  from a Plan 9 fs.
- The p9p command `mc` does not know the width of the Plan 9
  window, and uses the width of the terminal that `lxsrv` was
  started from.
- Insecure: no auth on server request; no encryption.

### Workaround for filesystem issues

Some Linux commands do file operations not supported on Plan
9 (symlinks, lockfiles), or not supported from a Linux mount
namespace (Linux `rename(2)`) (FIXME wasn't it, test with
simple program)

I encountered such problems with `go`, `rustc`, `git`.

The only solution is to run such commands on the native
Linux fs. To still get decent integration with Plan 9, I mount
my Linux home directory on Plan 9 using
[u9fs](https://bitbucket.org/plan9-from-bell-labs/u9fs/src/master/)
onto the same path as on Linux, so `/home/henri`.
Then if for example I do rust work, I run acme and shells
from `/home/henri` instead of `/usr/henri`, and from there
`lx cargo` works as expected. Not elegant but it works.

For this I installed u9fs on Linux, and I added the following
to my Plan 9 `$lib/profile`:
```
aux/stub -d /home
srv tcp!mylinuxhost!49151 u9fs # not using 9fs b/c of my custom port
mkdir -p /mnt/u9fs
mount -c /srv/u9fs /mnt/u9fs
bind /mnt/u9fs/home /home
```

Because you will forget to type `lx` before `go` or `cargo`,
you can create e.g. a `go` script which invokes `lx go`. See
example scripts under `./bin`.

Note that `git9` seems to be working fine on u9fs.

Troubleshooting
---------------

Using `lx` option `-d` will show debugging information that can
help troubleshoot problems.

The client and server maintain session log files
`/tmp/lx.$user/*.log`, resp.
`/tmp/lxsrv.$user/*.log` which contain the
same information printed out by the client when run with
option `-d`, plus some events occurring when there is no established
connection to the client's stderr. If you are having a problem with
VNC you may want to check its log under `~/.vnc`.

`lx` does not provide a pty - if you want a unix shell session or
terminal emulation, use `ssh(1)` and `vt(1)`.

`can't open display`: it may be that
the VNC server is too slow to start. Increase VNCWAITMS in
lxsrv.c. Note that this will increase the delay before get-
ting the X11 client displayed.

`unable to find a free vnc proxy port`: you killed the server
with SIGKILL or you encountered a bug. You can run on Linux the
command `rm /tmp/.X11-unix/X1??` to delete all VNC proxy sockets.
This may break X11 clients currently running under`lx`.

The VNC proxy sockets are `/tmp/.X11-unix/Xn`, with 100 <= n <= 199;
the real VNC sockets have 200 <= n <= 299.

Hacking
-------

This has been tested on a 9front kvm running on Arch Linux.

When working on `lx` I concurrently use two versions:
a stable version, built from the master branch; and a dev version,
built from another branch, in which the binaries have different
names. This allows me to use the stable version to build and
run the dev version.

The name `lx` is actually read from file `./name`. When working
on the dev branch, this file contains the string `lx2`, so that
the deployed binaries are `lx2` and `lx2srv`.

I keep two windows open:
- One running `mk watch`, to watch the files for change and
  trigger rebuilds
- Another running `mk srvloop`, which continuously runs `lx2srv`
  on port 8000, so as not to conflict with `lxsrv` which uses the
  default port of 9000. FIXME 9pfuse breaks if master lxsrv is
  already running?? Until fixed run from Linux:
  ``while(){ echo '####' `{date}; lx2srv -p 8000 || sleep 1 }``
