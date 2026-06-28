//! `hme-gateway`: a production Rust Gateway server fronting clients over
//! HTTP + WebSocket and bridging to the C++ Matching_Engine over TCP using the
//! shared binary Wire_Protocol.

mod api;
mod book;
mod engine_client;
mod state;
mod stats;

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;

use axum::routing::{get, post};
use axum::Router;
use tokio::net::TcpListener;
use tokio::sync::{broadcast, mpsc};
use tower_http::cors::CorsLayer;
use tower_http::services::{ServeDir, ServeFile};
use tower_http::trace::TraceLayer;

use crate::state::{AppState, COMMAND_CHANNEL_CAPACITY, EVENT_CHANNEL_CAPACITY};

/// Resolved runtime configuration.
struct Config {
    listen: String,
    engine: String,
    web: PathBuf,
}

impl Config {
    /// Resolve config from CLI flags, then `HME_*` env vars, then defaults.
    fn from_args() -> Config {
        let mut listen = None;
        let mut engine = None;
        let mut web = None;

        let mut args = std::env::args().skip(1);
        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--listen" => listen = args.next(),
                "--engine" => engine = args.next(),
                "--web" => web = args.next(),
                "-h" | "--help" => {
                    print_help();
                    std::process::exit(0);
                }
                other => {
                    eprintln!("warning: ignoring unknown argument '{other}'");
                }
            }
        }

        let listen = listen
            .or_else(|| std::env::var("HME_LISTEN").ok())
            .unwrap_or_else(|| "0.0.0.0:8080".to_string());
        let engine = engine
            .or_else(|| std::env::var("HME_ENGINE").ok())
            .unwrap_or_else(|| "127.0.0.1:9001".to_string());
        let web = web
            .or_else(|| std::env::var("HME_WEB").ok())
            .unwrap_or_else(|| "./web".to_string());

        Config {
            listen,
            engine,
            web: PathBuf::from(web),
        }
    }
}

fn print_help() {
    println!(
        "hme-gateway — Hyper-Match-Engine Gateway server\n\n\
         USAGE:\n    hme-gateway [--listen ADDR] [--engine ADDR] [--web DIR]\n\n\
         OPTIONS:\n    \
         --listen ADDR   HTTP/WS bind address (default 0.0.0.0:8080, env HME_LISTEN)\n    \
         --engine ADDR   Matching engine TCP address (default 127.0.0.1:9001, env HME_ENGINE)\n    \
         --web DIR       Static web root (default ./web, env HME_WEB)"
    );
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,tower_http=warn".into()),
        )
        .init();

    let config = Config::from_args();

    let (command_tx, command_rx) = mpsc::channel::<Vec<u8>>(COMMAND_CHANNEL_CAPACITY);
    let (events_tx, _events_rx) = broadcast::channel::<String>(EVENT_CHANNEL_CAPACITY);
    let state = Arc::new(AppState::new(command_tx, events_tx));

    // Persistent engine connection with reconnect.
    tokio::spawn(engine_client::run(
        state.clone(),
        config.engine.clone(),
        command_rx,
    ));

    // Background WebSocket pushers.
    tokio::spawn(api::book_broadcaster(state.clone()));
    tokio::spawn(api::stats_broadcaster(state.clone()));

    let app = build_router(state, &config.web);

    let listen_addr: SocketAddr = config.listen.parse()?;
    let listener = TcpListener::bind(listen_addr).await?;
    tracing::info!(
        listen = %listen_addr,
        engine = %config.engine,
        web = %config.web.display(),
        "hme-gateway listening"
    );

    axum::serve(listener, app)
        .with_graceful_shutdown(shutdown_signal())
        .await?;

    Ok(())
}

fn build_router(state: Arc<AppState>, web_dir: &PathBuf) -> Router {
    let index = web_dir.join("index.html");
    let static_files = ServeDir::new(web_dir).fallback(ServeFile::new(index));

    let api = Router::new()
        .route("/api/orders", post(api::post_order))
        .route("/api/cancel/:id", post(api::post_cancel))
        .route("/api/book", get(api::get_book))
        .route("/api/stats", get(api::get_stats))
        .route("/api/trades", get(api::get_trades))
        .layer(CorsLayer::permissive());

    Router::new()
        .merge(api)
        .route("/ws", get(api::ws_handler))
        .fallback_service(static_files)
        .layer(TraceLayer::new_for_http())
        .with_state(state)
}

/// Resolve on Ctrl-C (and SIGTERM on Unix) for graceful shutdown.
async fn shutdown_signal() {
    let ctrl_c = async {
        let _ = tokio::signal::ctrl_c().await;
    };

    #[cfg(unix)]
    let terminate = async {
        if let Ok(mut sig) =
            tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
        {
            sig.recv().await;
        }
    };

    #[cfg(not(unix))]
    let terminate = std::future::pending::<()>();

    tokio::select! {
        _ = ctrl_c => {}
        _ = terminate => {}
    }

    tracing::info!("shutdown signal received");
}
