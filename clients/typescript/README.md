# hme-client (TypeScript)

A small, dependency-free TypeScript client for the Hyper-Match-Engine gateway's
HTTP + WebSocket API. Runs in the browser and in Node 18+ on the global `fetch`
and `WebSocket` — no runtime dependencies.

## Install / build

```bash
cd clients/typescript
npm install
npm run build   # emits dist/ (ESM + .d.ts)
```

In an app you would publish/link this package and `import` from `hme-client`.

## Usage

```ts
import { HmeClient, subscribe } from "hme-client";

const client = new HmeClient("http://127.0.0.1:8080", { apiKey: "optional" });

const resting = await client.sell(101.0, 10);     // OrderAck
const crossing = await client.buy(101.0, 4);      // crosses the resting sell
console.log(crossing.accepted, crossing.filled, crossing.fills.map((f) => f.price));

await client.cancel(resting.orderId!);            // CancelResult
const book = await client.book(5);                // { bids, asks, sequence }
const stats = await client.stats();

// Live event stream over WebSocket
const sub = subscribe("http://127.0.0.1:8080", (event) => {
  if (event.type === "trade") console.log("trade", event.price, event.quantity);
});
// sub.close() to stop
```

## Result conventions

`submitOrder` / `buy` / `sell` never throw on a business rejection — they return
an `OrderAck`. Branch on `ack.accepted`:

- `accepted === true` → use `ack.orderId`, `ack.filled`, `ack.resting`, `ack.fills`.
- `accepted === false` → read `ack.reason` (e.g. `"duplicate_order_id"`,
  `"engine_unavailable"`) and `ack.field`.

`ack.httpStatus` and `ack.raw` expose the raw status and JSON. `cancel` returns a
`CancelResult` with `.cancelled` and `.reason`. Only transport failures,
unparseable responses, and HTTP 401 throw (`HmeTransportError`,
`HmeProtocolError`, `HmeAuthError`, all extending `HmeError`).

## Authentication

When the gateway runs with API keys, mutating calls require an `X-API-Key`
header — pass `apiKey` to the constructor. The `/ws` feed is unauthenticated, so
streaming needs no key.

## Example

`examples/quickstart.mjs` runs against a local gateway after `npm run build`:

```bash
node examples/quickstart.mjs
```
