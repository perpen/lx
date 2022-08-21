FROM archlinux as builder
RUN pacman -Syu --noconfirm && pacman -S --noconfirm sudo
COPY container/install-p9p /
RUN /install-p9p
COPY container/install-tigervnc /
RUN /install-tigervnc
COPY . /lx
RUN /lx/container/install-lx
RUN /lx/container/install-dwm

FROM archlinux
COPY --from=builder /usr/local/tigervnc /usr/local/tigervnc
COPY --from=builder /usr/local/p9p /usr/local/p9p
RUN pacman -Syu --noconfirm && pacman -S --noconfirm sudo fuse2
RUN pacman -S --noconfirm \
    fltk pam gnutls libjpeg-turbo libxtst pixman \
    xorg-xauth xorg-xsetroot xkeyboard-config xorg-xkbcomp \
    libgl libgcrypt perl libxdamage libxfont2 libdrm \
    xorg-xinit xorg-xhost xorg-xsetroot xterm terminus-font xorg-xeyes
RUN echo '%wheel ALL=(ALL:ALL) NOPASSWD: ALL' >> /etc/sudoers \
    && echo "export PLAN9=/usr/local/p9p" >/etc/profile.d/p9p.sh \
    && echo "export PATH=\$PATH:/usr/local/p9p/bin" >>/etc/profile.d/p9p.sh \
    && mkdir /tmp/.X11-unix \
    && chmod +t /tmp/.X11-unix
COPY container/entrypoint /entrypoint
ENTRYPOINT ["/entrypoint"]
