# Multi-stage build: compiler image -> small runtime image with fpx + fpx-server.
#   docker build -f docker/Dockerfile -t fp-extractor .
#   docker run -p 8080:8080 -v fpx-data:/data fp-extractor
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFPX_BUILD_TESTS=ON -Wno-dev \
 && cmake --build build -j"$(nproc)" \
 && (cd build && ctest --output-on-failure) \
 && cmake --install build --prefix /opt/fpx

FROM ubuntu:24.04
RUN useradd --system --uid 10001 fpx && mkdir /data && chown fpx /data
COPY --from=build /opt/fpx /opt/fpx
ENV PATH=/opt/fpx/bin:$PATH
USER fpx
VOLUME /data
EXPOSE 8080
HEALTHCHECK CMD ["/bin/sh", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 || exit 1"]
ENTRYPOINT ["fpx-server", "--db", "/data/gallery.fpxdb", "--port", "8080"]
