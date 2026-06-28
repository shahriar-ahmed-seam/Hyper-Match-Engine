import { HmeAuthError, HmeProtocolError, HmeTransportError } from "./errors.js";
import type {
  BookSnapshot,
  CancelResult,
  Fill,
  OrderAck,
  Side,
  Stats,
  Trade,
} from "./types.js";

export interface HmeClientOptions {
  /** API key sent as `X-API-Key`; required by mutating endpoints when the gateway has auth enabled. */
  apiKey?: string;
  /** Per-request timeout in milliseconds. */
  timeoutMs?: number;
}

interface OrderRequest {
  side: Side;
  price: number;
  quantity: number;
  orderId?: number;
}

type Json = Record<string, unknown>;

/**
 * Client for the Hyper-Match-Engine gateway's HTTP API. Works in the browser
 * and in Node 18+ using the global `fetch`; it has no runtime dependencies.
 *
 * Business rejections (duplicate id, invalid field, unknown order) are returned
 * as typed results, not thrown. Only transport failures, unparseable bodies,
 * and HTTP 401 throw ({@link HmeTransportError}, {@link HmeProtocolError},
 * {@link HmeAuthError}).
 */
export class HmeClient {
  private readonly baseUrl: string;
  private readonly apiKey?: string;
  private readonly timeoutMs: number;

  constructor(baseUrl = "http://127.0.0.1:8080", options: HmeClientOptions = {}) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.apiKey = options.apiKey;
    this.timeoutMs = options.timeoutMs ?? 5000;
  }

  /** Submit a new order. Resolves to an {@link OrderAck}; check `accepted`. */
  async submitOrder(order: OrderRequest): Promise<OrderAck> {
    const payload: Json = {
      side: order.side,
      price: order.price,
      quantity: order.quantity,
    };
    if (order.orderId !== undefined) payload.order_id = order.orderId;

    const { status, body } = await this.request("POST", "/api/orders", { json: payload });
    const fills: Fill[] = Array.isArray(body.fills)
      ? (body.fills as Json[]).map((f) => ({
          price: f.price as number,
          quantity: f.quantity as number,
          restingId: f.resting_id as number,
          execSeq: f.exec_seq as number,
        }))
      : [];

    return {
      accepted: body.status === "accepted",
      httpStatus: status,
      orderId: body.order_id as number | undefined,
      side: body.side as Side | undefined,
      price: body.price as number | undefined,
      quantity: body.quantity as number | undefined,
      filled: (body.filled as number) ?? 0,
      resting: (body.resting as number) ?? 0,
      fills,
      latencyUs: body.latency_us as number | undefined,
      reason: body.reason as string | undefined,
      field: body.field as string | undefined,
      raw: body,
    };
  }

  /** Submit a buy order. */
  buy(price: number, quantity: number, orderId?: number): Promise<OrderAck> {
    return this.submitOrder({ side: "buy", price, quantity, orderId });
  }

  /** Submit a sell order. */
  sell(price: number, quantity: number, orderId?: number): Promise<OrderAck> {
    return this.submitOrder({ side: "sell", price, quantity, orderId });
  }

  /** Cancel a resting order by id. */
  async cancel(orderId: number): Promise<CancelResult> {
    const { status, body } = await this.request("POST", `/api/cancel/${orderId}`);
    return {
      cancelled: body.status === "cancelled",
      httpStatus: status,
      orderId: body.order_id as number | undefined,
      latencyUs: body.latency_us as number | undefined,
      reason: body.reason as string | undefined,
      raw: body,
    };
  }

  /** Fetch a depth-limited order book snapshot. */
  async book(depth = 15): Promise<BookSnapshot> {
    const { body } = await this.request("GET", "/api/book", { query: { depth } });
    return body as unknown as BookSnapshot;
  }

  /** Fetch gateway + engine statistics. */
  async stats(): Promise<Stats> {
    const { body } = await this.request("GET", "/api/stats");
    return body as unknown as Stats;
  }

  /** Fetch the most recent trades, newest first. */
  async trades(limit = 50): Promise<Trade[]> {
    const { body } = await this.request("GET", "/api/trades", { query: { limit } });
    return body as unknown as Trade[];
  }

  private async request(
    method: string,
    path: string,
    opts: { json?: Json; query?: Record<string, string | number> } = {},
  ): Promise<{ status: number; body: any }> {
    const url = new URL(this.baseUrl + path);
    for (const [k, v] of Object.entries(opts.query ?? {})) {
      url.searchParams.set(k, String(v));
    }

    const headers: Record<string, string> = {};
    if (opts.json) headers["Content-Type"] = "application/json";
    if (this.apiKey) headers["X-API-Key"] = this.apiKey;

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let response: Response;
    try {
      response = await fetch(url, {
        method,
        headers,
        body: opts.json ? JSON.stringify(opts.json) : undefined,
        signal: controller.signal,
      });
    } catch (err) {
      throw new HmeTransportError(`request to ${url.toString()} failed: ${String(err)}`);
    } finally {
      clearTimeout(timer);
    }

    if (response.status === 401) {
      throw new HmeAuthError();
    }

    let body: unknown;
    try {
      body = await response.json();
    } catch (err) {
      throw new HmeProtocolError(
        `non-JSON response from ${url.toString()} (status ${response.status}): ${String(err)}`,
      );
    }
    return { status: response.status, body };
  }
}
