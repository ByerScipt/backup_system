FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBACKUP_BUILD_GUI=OFF && cmake --build build -j2

FROM ubuntu:24.04
COPY --from=build /src/build/backup-cli /usr/local/bin/backup-cli
COPY --from=build /src/build/backup-server /usr/local/bin/backup-server
EXPOSE 8848
ENTRYPOINT ["backup-server"]

