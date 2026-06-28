# Matching engine (C++) — multi-stage build.
FROM gcc:13 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY cpp ./cpp
# Tests (Catch2/RapidCheck) are skipped here; we only need the server binary.
# libstdc++/libgcc are linked statically so the runtime image stays minimal.
RUN cmake -S cpp -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DHME_BUILD_TESTS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
 && cmake --build build --target hme_engine_server

FROM debian:bookworm-slim AS runtime
COPY --from=build /src/build/server/hme_engine_server /usr/local/bin/hme-engine
EXPOSE 9001
ENTRYPOINT ["/usr/local/bin/hme-engine"]
CMD ["--host", "0.0.0.0", "--port", "9001"]
