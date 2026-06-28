export type Side = "buy" | "sell";

/** A single execution produced when an incoming order crosses the book. */
export interface Fill {
  price: number;
  quantity: number;
  restingId: number;
  execSeq: number;
}

/** Outcome of {@link HmeClient.submitOrder}. Inspect `accepted` to branch. */
export interface OrderAck {
  accepted: boolean;
  httpStatus: number;
  orderId?: number;
  side?: Side;
  price?: number;
  quantity?: number;
  filled: number;
  resting: number;
  fills: Fill[];
  latencyUs?: number;
  /** Set when `accepted` is false: e.g. "duplicate_order_id", "engine_unavailable". */
  reason?: string;
  /** Offending field when the gateway provides it (validation rejections). */
  field?: string;
  /** Unmodified JSON body. */
  raw: Record<string, unknown>;
}

/** Outcome of {@link HmeClient.cancel}. */
export interface CancelResult {
  cancelled: boolean;
  httpStatus: number;
  orderId?: number;
  latencyUs?: number;
  /** Set when `cancelled` is false: "order_not_found" or "no_longer_resting". */
  reason?: string;
  raw: Record<string, unknown>;
}

/** One aggregated price level in the order book. */
export interface Level {
  price: number;
  quantity: number;
  orders: number;
}

/** A depth-limited order book snapshot (bids high→low, asks low→high). */
export interface BookSnapshot {
  bids: Level[];
  asks: Level[];
  sequence: number;
}

export interface LatencyStats {
  p50: number;
  p99: number;
  max: number;
}

/** Gateway + engine statistics (field names match the REST response). */
export interface Stats {
  engine_connected: boolean;
  orders_submitted: number;
  orders_accepted: number;
  orders_rejected: number;
  cancels: number;
  trades: number;
  resting_orders: number;
  latency_us: LatencyStats;
  throughput_ops: number;
  auth_required: boolean;
}

/** A trade print (field names match the REST/WS response). */
export interface Trade {
  exec_seq: number;
  price: number;
  quantity: number;
  incoming_id: number;
  resting_id: number;
  ts_ms: number;
}

export interface TradeEvent extends Trade {
  type: "trade";
}
export interface AcceptedEvent {
  type: "accepted";
  order_id: number;
  side: Side;
  price: number;
  quantity: number;
  filled: number;
  resting: number;
}
export interface RejectedEvent {
  type: "rejected";
  order_id: number;
  reason: string;
}
export interface CancelledEvent {
  type: "cancelled";
  order_id: number;
}
export interface BookEvent {
  type: "book";
  bids: Level[];
  asks: Level[];
  sequence?: number;
}
export interface StatsEvent extends Partial<Stats> {
  type: "stats";
}

/** Any event delivered over the WebSocket feed. */
export type EngineEvent =
  | TradeEvent
  | AcceptedEvent
  | RejectedEvent
  | CancelledEvent
  | BookEvent
  | StatsEvent;
