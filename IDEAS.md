Bugs:
- lx2srv not working from srvloop? if lxsrv is running, maybe.
  if run remotely??
- fix slay lx2 on bash -i, server should cleanup on connection break
- ports: openbsd doesn't have mount namespaces. freebsd?

Maybe:
- security: auth to lxsrv? use tls for encryption? and tls
  for auth?

```
% aux/listen1 -tv tcp!*!2000 exportfs -dr / <[10=0] >[11=1] >[12=2]
$ rc -i   </9/fd/10 >>/9/fd/11  2>>/9/fd/12
```
