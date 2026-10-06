# M10 packaging: archlinux makepkg builder/smoker.
FROM archlinux:base-devel
RUN pacman -Syu --noconfirm --needed \
        cmake glfw mesa \
        xorg-server-xvfb libglvnd xf86-video-dummy \
    && pacman -Scc --noconfirm
