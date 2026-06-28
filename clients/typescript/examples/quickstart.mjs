// Quickstart for the Hyper-Match-Engine TypeScript client (Node 18+).
//
//   cd clients/typescript && npm install && npm run build
//   node examples/quickstart.mjs
//
// Set HME_BASE_URL / HME_API_KEY to override the defaults.

import { HmeClient, subscribe } from "../dist/index.js";

const baseUrl = process.env.HME_BASE_URL ?? "http://127.0.0.1:8080";
const apiKey = process.env.HME_API_KEY || undefined;

const client = new HmeClient(baseUrl, { apiKey });

// Stream live events for a few seconds.
const sub = subscribe(baseUrl, (event) => {
  if (event.type === "trade" || event.type === "accepted") {
    console.log("event:", event);
  }
});

// Rest a sell, then cross it with a buy.
const resting = await client.sell(101.0, 10);
console.log(`resting sell -> accepted=${resting.accepted} id=${resting.orderId} resting=${resting.resting}`);

const crossing = await client.buy(101.0, 4);
console.log(`crossing buy -> accepted=${crossing.accepted} filled=${crossing.filled}`);
for (const fill of crossing.fills) {
  console.log(`  fill ${fill.quantity} @ ${fill.price} (resting_id=${fill.restingId}, exec_seq=${fill.execSeq})`);
}

if (resting.orderId !== undefined) {
  const cancelled = await client.cancel(resting.orderId);
  console.log(`cancel ${resting.orderId} -> cancelled=${cancelled.cancelled} reason=${cancelled.reason ?? "-"}`);
}

const book = await client.book(5);
console.log("top of book:", book.bids[0], book.asks[0], "seq", book.sequence);

const stats = await client.stats();
console.log(`stats -> accepted=${stats.orders_accepted} trades=${stats.trades} resting=${stats.resting_orders}`);

setTimeout(() => {
  sub.close();
  process.exit(0);
}, 3000);
