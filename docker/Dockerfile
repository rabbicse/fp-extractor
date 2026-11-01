# Multi-stage build: compiler image -> small runtime image with fpx + fpx-server.
#   docker build -f docker/Dockerfile -t fp-extractor .
#   docker run -p 8080:8080 -e FPX_API_KEY=change-me -v fpx-data:/data fp-extractor
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFPX_BUILD_TESTS=ON -Wno-dev \
 && cmake --build build -j"$(nproc)" \
 && (cd build && ctest --output-on-failure) \
 && cmake --install build --prefix /opt/fpx

FROM ubuntu:24.04
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="fp-extractor" \
      org.opencontainers.image.description="Fingerprint WSQ / ANSI-378 / ISO-19794-2 extraction, matching and 1:N identification server" \
      org.opencontainers.image.source="https://github.com/rabbicse/fp-extractor" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}"
RUN useradd --system --uid 10001 fpx && mkdir /data && chown fpx /data
COPY --from=build /opt/fpx /opt/fpx
ENV PATH=/opt/fpx/bin:$PATH
USER fpx
VOLUME /data
EXPOSE 8080
# bash (not sh/dash) is needed for /dev/tcp; checks that /healthz answers "ok".
HEALTHCHECK --interval=15s --timeout=5s --start-period=120s --retries=3 \
  CMD ["/bin/bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 && printf 'GET /healthz HTTP/1.0\\r\\n\\r\\n' >&3 && grep -q '\"ok\"' <&3"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["fpx-server", "--db", "/data/gallery.fpxdb", "--port", "8080"]
