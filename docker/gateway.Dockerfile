# Gateway + web console (Rust) — multi-stage build.
FROM rust:1-bookworm AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY codec ./codec
COPY gateway ./gateway
COPY gateway-server ./gateway-server
RUN cargo build --release -p gateway-server

FROM debian:bookworm-slim AS runtime
WORKDIR /app
COPY --from=build /src/target/release/hme-gateway /usr/local/bin/hme-gateway
COPY web ./web
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/hme-gateway"]
CMD ["--listen", "0.0.0.0:8080", "--engine", "engine:9001", "--web", "/app/web"]
