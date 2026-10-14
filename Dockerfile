# Server-only image for the separated deployment line; the Qt client is a
# desktop package. Build from the repository root:
#   docker build -t backup-studio-server:1.8.0 \
#       --build-arg BACKUP_VERSION=1.8.0 .
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake g++ make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DBACKUP_BUILD_GUI=OFF -DBACKUP_BUILD_TESTS=OFF \
 && cmake --build build --parallel 2

FROM ubuntu:24.04
ARG BACKUP_VERSION=dev
LABEL org.opencontainers.image.title="backup-studio-server" \
      org.opencontainers.image.description="Backup Studio archive server (NBKP over TCP, no TLS)" \
      org.opencontainers.image.version="${BACKUP_VERSION}"

COPY --from=build /src/bin/backup-server /usr/local/bin/backup-server
COPY --from=build /src/bin/backup-cli /usr/local/bin/backup-cli
COPY --from=build /src/packaging/databackup.conf \
     /etc/backup-studio/databackup.conf

# The service runs unprivileged. storage_path in the shipped configuration is
# relative, so the archives land in /data/server_data inside the volume below.
RUN useradd --system --uid 10001 --user-group --home-dir /data \
        --shell /usr/sbin/nologin backup-studio \
 && mkdir -p /data \
 && chown backup-studio:backup-studio /data
USER backup-studio
WORKDIR /data
VOLUME /data
EXPOSE 8848
ENTRYPOINT ["backup-server"]
CMD ["--config", "/etc/backup-studio/databackup.conf"]
