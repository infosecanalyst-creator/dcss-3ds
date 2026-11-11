# Cross-build environment for DCSS on the Nintendo 3DS.
#
#   docker build -t dcss-3ds .
#   docker run --rm -v "$PWD:/work" dcss-3ds ./build-3ds.sh
#
# devkitpro/devkitarm already contains devkitARM, libctru and the 3DS tools
# (3dsxtool, smdhtool). We add the host tools crawl's build needs.
FROM devkitpro/devkitarm:latest

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential git perl python3 python3-yaml flex bison pkg-config \
        libpng-dev zlib1g-dev python3-numpy python3-pil ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# makerom (for the .cia) is not in the image. Optional; the .3dsx does not need it.
# RUN git clone --depth 1 https://github.com/3DSGuy/Project_CTR /tmp/ctr && \
#     make -C /tmp/ctr/makerom && cp /tmp/ctr/makerom/bin/makerom /usr/local/bin/

ENV DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
WORKDIR /work
