# 多阶段构建：build 阶段带全套 dev 依赖，runtime 只留运行库
FROM debian:bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake g++ make ca-certificates \
      libsqlite3-dev libmariadb-dev libhiredis-dev librdkafka-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DDWS_BUILD_TESTS=OFF -DDWS_HAVE_DCMTK=OFF \
      -DDWS_HAVE_MYSQL=ON -DDWS_HAVE_REDIS=ON -DDWS_HAVE_KAFKA=ON \
 && cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------- runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libsqlite3-0 libmariadb3 libhiredis1.1.0 librdkafka1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/dicomweb-server /app/dicomweb-server

VOLUME ["/data"]
EXPOSE 8080
ENTRYPOINT ["/app/dicomweb-server"]
CMD ["--port", "8080", "--data", "/data"]
