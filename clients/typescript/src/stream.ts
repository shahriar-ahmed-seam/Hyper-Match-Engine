import type { EngineEvent } from "./types.js";

export interface SubscribeOptions {
  /** Reconnect automatically after a dropped connection (default true). */
  reconnect?: boolean;
  /** Delay between reconnect attempts, in milliseconds (default 1000). */
  reconnectDelayMs?: number;
  onOpen?: () => void;
  onClose?: () => void;
  onError?: (err: unknown) => void;
}

/** Handle returned by {@link subscribe}; call `close()` to stop streaming. */
export interface Subscription {
  close(): void;
}

function wsUrl(baseUrl: string): string {
  const url = baseUrl.replace(/\/+$/, "");
  if (url.startsWith("https://")) return "wss://" + url.slice("https://".length) + "/ws";
  if (url.startsWith("http://")) return "ws://" + url.slice("http://".length) + "/ws";
  return url + "/ws";
}

/**
 * Subscribe to the gateway's `/ws` event feed. `onEvent` is invoked with each
 * parsed {@link EngineEvent} (`trade`, `accepted`, `rejected`, `cancelled`,
 * `book`, `stats`). Uses the global `WebSocket` (browser and Node 21+).
 *
 * The `/ws` endpoint is unauthenticated, so no API key is required for streaming.
 *
 * @returns a {@link Subscription}; call `close()` to stop (and disable reconnect).
 */
export function subscribe(
  baseUrl: string,
  onEvent: (event: EngineEvent) => void,
  options: SubscribeOptions = {},
): Subscription {
  const url = wsUrl(baseUrl);
  const reconnect = options.reconnect ?? true;
  const delay = options.reconnectDelayMs ?? 1000;

  let socket: WebSocket | null = null;
  let closed = false;
  let timer: ReturnType<typeof setTimeout> | null = null;

  const connect = () => {
    if (closed) return;
    socket = new WebSocket(url);

    socket.onopen = () => options.onOpen?.();
    socket.onmessage = (ev: MessageEvent) => {
      try {
        const event = JSON.parse(String(ev.data)) as EngineEvent;
        if (event && typeof event.type === "string") onEvent(event);
      } catch {
        /* ignore non-JSON frames */
      }
    };
    socket.onerror = (err) => options.onError?.(err);
    socket.onclose = () => {
      options.onClose?.();
      if (!closed && reconnect) {
        timer = setTimeout(connect, delay);
      }
    };
  };

  connect();

  return {
    close() {
      closed = true;
      if (timer) clearTimeout(timer);
      socket?.close();
    },
  };
}
