# Build image

`docker build -f container/Dockerfile -t lx .`

# Run

Ports:
- 9000: lxsrv port
- 6100-6015: vnc server

```
ifc=192.168.0.2; docker run --rm \
  -p $ifc:9000:9000 \
  -p $ifc:6100-6109:6100-6109 \
  --cap-add SYS_ADMIN --device /dev/fuse lx henri
```
