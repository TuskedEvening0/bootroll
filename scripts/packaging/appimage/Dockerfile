# M10 packaging: AppImage builder (ubuntu:22.04 = glibc 2.35 floor).
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ make cmake git libglfw3-dev libgl1-mesa-dev ca-certificates curl file \
        xvfb libgl1 libglx-mesa0 libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*
