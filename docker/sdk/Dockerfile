ARG VIVADO_EDITION=webpack
ARG PSL=PSL8

FROM metalfs/sdk-base:$VIVADO_EDITION AS build

ADD . /src/metal_fs
RUN cd /src/metal_fs \
 && mkdir build \
 && cd build \
 && cmake \
    -DCMAKE_INSTALL_PREFIX=/install/ \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPTION_BUILD_TESTS=OFF .. \
    -DPSL_VERSION=$PSL
 && make && make install

FROM metalfs/sdk-base:$VIVADO_EDITION

COPY --from=build /install/metal-driver /usr/local/bin/
COPY --from=build /install/metal-driver-placeholder /usr/local/bin/
COPY --from=build /install/lib/ /usr/local/lib/

RUN ldconfig

ADD . /sdk/metal_fs
ENV METAL_ROOT=/sdk/metal_fs