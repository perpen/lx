# For fuse:
#   docker run --rm --cap-add SYS_ADMIN --device /dev/fuse -ti x bash

FROM archlinux as builder
RUN pacman -Syu --noconfirm
COPY provision /
RUN bash -x /provision build
EXPOSE 9000
ENV PLAN9=/usr/local/p9p
ENV PATH=$PATH:$PLAN9/bin

#FROM archlinux
