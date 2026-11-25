# ---------- build ----------
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make libssl-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt ./
COPY third_party ./third_party
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j2

# ---------- runtime ----------
FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 zlib1g ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /app/build/pickems-prediction /app/pickems-prediction
COPY public /app/public
ENV DATA_DIR=/data \
    PUBLIC_DIR=/app/public \
    PORT=8080
RUN mkdir -p /data
EXPOSE 8080
CMD ["/app/pickems-prediction"]
