"use strict";

const $ = (id) => document.getElementById(id);
const BOOK_DEPTH = 14;
const TAPE_MAX = 120;
const SPARK_MAX = 90;
const POLL_MS = 1000;

const state = {
  book: { bids: [], asks: [], sequence: null },
  trades: [],
  lastTradePrice: null,
  spark: [],
  side: "buy",
  wsLive: false,
};

/* ---------- formatting ---------- */
const nfInt = new Intl.NumberFormat("en-US");
const fmtInt = (n) => nfInt.format(Math.round(Number(n) || 0));
const fmtPrice = (n) => Number(n).toFixed(2);
const nowTime = () => new Date().toLocaleTimeString("en-US", { hour12: false });
const tsTime = (ms) => new Date(ms).toLocaleTimeString("en-US", { hour12: false });

/* ---------- connection / transport indicators ---------- */
function setConnected(up) {
  $("connDot").className = "conn-dot " + (up ? "up" : "down");
  $("connText").textContent = up ? "engine connected" : "engine disconnected";
}
function setTransport(ws) {
  state.wsLive = ws;
  const el = $("linkState");
  el.textContent = ws ? "ws live" : "poll";
  el.className = "link-state" + (ws ? " ws" : "");
}

/* ---------- metrics + sparkline ---------- */
function renderStats(s) {
  if (!s) return;
  setConnected(!!s.engine_connected);
  const lat = s.latency_us || { p50: 0, p99: 0, max: 0 };
  $("mThroughput").textContent = fmtInt(s.throughput_ops);
  $("mP50").textContent = fmtInt(lat.p50);
  $("mP99").textContent = fmtInt(lat.p99);
  $("mMax").textContent = fmtInt(lat.max);
  $("mSubmitted").textContent = fmtInt(s.orders_submitted);
  $("mAccepted").textContent = fmtInt(s.orders_accepted);
  $("mRejected").textContent = fmtInt(s.orders_rejected);
  $("mCancels").textContent = fmtInt(s.cancels);
  $("mTrades").textContent = fmtInt(s.trades);
  $("mResting").textContent = fmtInt(s.resting_orders);
  pushSpark(Number(lat.p50) || 0);
}

function pushSpark(v) {
  state.spark.push(v);
  if (state.spark.length > SPARK_MAX) state.spark.shift();
  drawSpark();
}

function drawSpark() {
  const cv = $("spark");
  const ctx = cv.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth || 320;
  const h = cv.clientHeight || 44;
  if (cv.width !== w * dpr || cv.height !== h * dpr) {
    cv.width = w * dpr;
    cv.height = h * dpr;
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const data = state.spark;
  if (data.length < 2) return;
  const max = Math.max(...data, 1);
  const min = Math.min(...data, 0);
  const range = max - min || 1;
  const stepX = w / (SPARK_MAX - 1);
  const x0 = w - (data.length - 1) * stepX;
  const y = (v) => h - 3 - ((v - min) / range) * (h - 6);

  ctx.beginPath();
  data.forEach((v, i) => {
    const px = x0 + i * stepX;
    const py = y(v);
    i === 0 ? ctx.moveTo(px, py) : ctx.lineTo(px, py);
  });
  const grad = ctx.createLinearGradient(0, 0, w, 0);
  grad.addColorStop(0, "rgba(77,159,255,0.4)");
  grad.addColorStop(1, "#4d9fff");
  ctx.strokeStyle = grad;
  ctx.lineWidth = 1.5;
  ctx.lineJoin = "round";
  ctx.stroke();

  ctx.lineTo(x0 + (data.length - 1) * stepX, h);
  ctx.lineTo(x0, h);
  ctx.closePath();
  ctx.fillStyle = "rgba(77,159,255,0.08)";
  ctx.fill();
}

/* ---------- order book ---------- */
function renderBook(book) {
  if (!book) return;
  state.book = book;
  const bids = (book.bids || []).slice(0, BOOK_DEPTH);
  const asks = (book.asks || []).slice(0, BOOK_DEPTH);
  $("seq").textContent = book.sequence != null ? fmtInt(book.sequence) : "—";

  const maxQty = Math.max(
    1,
    ...bids.map((l) => l.quantity),
    ...asks.map((l) => l.quantity)
  );

  // asks render low-at-bottom: highest ask on top, best ask just above mid
  const asksDesc = asks.slice().sort((a, b) => b.price - a.price);
  $("asks").innerHTML = asksDesc.map((l) => ladderRow(l, "ask", maxQty)).join("");
  $("bids").innerHTML = bids.map((l) => ladderRow(l, "bid", maxQty)).join("");

  const bestBid = bids.length ? bids[0].price : null;
  const bestAsk = asks.length ? asks[0].price : null;
  $("bestBid").textContent = bestBid != null ? fmtPrice(bestBid) : "—";
  $("bestAsk").textContent = bestAsk != null ? fmtPrice(bestAsk) : "—";

  if (bestBid != null && bestAsk != null) {
    const spread = bestAsk - bestBid;
    const mid = (bestAsk + bestBid) / 2;
    $("spread").textContent = fmtPrice(spread);
    $("mid").textContent = fmtPrice(mid);
    $("ladderMid").textContent = `mid ${fmtPrice(mid)}  ·  spread ${fmtPrice(spread)}`;
  } else {
    $("spread").textContent = "—";
    $("mid").textContent = "—";
    $("ladderMid").textContent = "—";
  }
}

function ladderRow(level, side, maxQty) {
  const pct = Math.max(2, (level.quantity / maxQty) * 100);
  return (
    `<div class="ladder-row ${side}">` +
    `<span class="price">${fmtPrice(level.price)}</span>` +
    `<span class="qty">${fmtInt(level.quantity)}</span>` +
    `<span class="orders">${fmtInt(level.orders)}</span>` +
    `<span class="depth"><span class="depth-fill" style="width:${pct}%"></span></span>` +
    `</div>`
  );
}

/* ---------- trade tape ---------- */
function addTrade(t, flash) {
  state.trades.unshift(t);
  if (state.trades.length > TAPE_MAX) state.trades.pop();
  renderTape(flash);
}

function renderTape(flashTop) {
  const rows = state.trades;
  let html = "";
  for (let i = 0; i < rows.length; i++) {
    const t = rows[i];
    const prev = rows[i + 1];
    const dir = prev ? (t.price >= prev.price ? "up" : "down") : "up";
    const flash = i === 0 && flashTop ? " flash" : "";
    html +=
      `<div class="tape-row ${dir}${flash}">` +
      `<span class="time">${tsTime(t.ts_ms)}</span>` +
      `<span class="price">${fmtPrice(t.price)}</span>` +
      `<span class="qty">${fmtInt(t.quantity)}</span>` +
      `<span class="seq">#${t.exec_seq}</span>` +
      `</div>`;
  }
  $("tape").innerHTML = html;
  $("tapeCount").textContent = rows.length ? `${rows.length} prints` : "";
  if (rows.length) state.lastTradePrice = rows[0].price;
}

/* ---------- event log ---------- */
function log(kind, tag, msg) {
  const el = $("log");
  const row = document.createElement("div");
  row.className = "log-row " + kind;
  row.innerHTML =
    `<span class="ts">${nowTime()}</span>` +
    `<span class="tag">${tag}</span>` +
    `<span class="msg">${msg}</span>`;
  el.prepend(row);
  while (el.childElementCount > 200) el.removeChild(el.lastChild);
}
$("clearLog").addEventListener("click", () => ($("log").innerHTML = ""));

/* ---------- REST helpers ---------- */
async function getJSON(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error(`${path} → ${r.status}`);
  return r.json();
}

async function postOrder(body) {
  const r = await fetch("/api/orders", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  let data = null;
  try { data = await r.json(); } catch { /* non-json */ }
  return { status: r.status, data };
}

async function postCancel(id) {
  const r = await fetch(`/api/cancel/${id}`, { method: "POST" });
  let data = null;
  try { data = await r.json(); } catch { /* non-json */ }
  return { status: r.status, data };
}

/* ---------- polling fallback ---------- */
let pollTimer = null;
async function pollOnce() {
  try {
    const [book, stats, trades] = await Promise.all([
      getJSON(`/api/book?depth=${BOOK_DEPTH}`),
      getJSON("/api/stats"),
      getJSON(`/api/trades?limit=${TAPE_MAX}`),
    ]);
    renderBook(book);
    renderStats(stats);
    state.trades = (trades || []).slice(0, TAPE_MAX);
    renderTape(false);
  } catch {
    setConnected(false);
  }
}
function startPolling() {
  if (pollTimer) return;
  pollOnce();
  pollTimer = setInterval(pollOnce, POLL_MS);
}
function stopPolling() {
  if (!pollTimer) return;
  clearInterval(pollTimer);
  pollTimer = null;
}

/* ---------- websocket ---------- */
let ws = null;
let wsRetry = null;
function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  try {
    ws = new WebSocket(`${proto}://${location.host}/ws`);
  } catch {
    scheduleReconnect();
    return;
  }

  ws.onopen = () => {
    setTransport(true);
    stopPolling();
    log("info", "LINK", "websocket connected");
  };
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch { return; }
    handleWsMessage(m);
  };
  ws.onclose = () => {
    setTransport(false);
    startPolling();
    scheduleReconnect();
  };
  ws.onerror = () => { try { ws.close(); } catch { /* ignore */ } };
}
function scheduleReconnect() {
  if (wsRetry) return;
  wsRetry = setTimeout(() => { wsRetry = null; connectWs(); }, 2000);
}

function handleWsMessage(m) {
  switch (m.type) {
    case "book":
      renderBook({ bids: m.bids, asks: m.asks, sequence: m.sequence });
      break;
    case "stats":
      renderStats(m);
      break;
    case "trade":
      addTrade(
        {
          exec_seq: m.exec_seq,
          price: m.price,
          quantity: m.quantity,
          incoming_id: m.incoming_id,
          resting_id: m.resting_id,
          ts_ms: m.ts_ms || Date.now(),
        },
        true
      );
      break;
    case "accepted":
      log("accepted", "ACCEPTED", `#${m.order_id} ${m.side} ${fmtPrice(m.price)} ×${m.quantity} · filled ${m.filled} resting ${m.resting}`);
      break;
    case "rejected":
      log("rejected", "REJECTED", `#${m.order_id ?? "?"} · ${m.reason}`);
      break;
    case "cancelled":
      log("cancelled", "CANCELLED", `#${m.order_id}`);
      break;
    default:
      break;
  }
}

/* ---------- order entry ---------- */
function setSide(side) {
  state.side = side;
  $("sideBuy").classList.toggle("active", side === "buy");
  $("sideSell").classList.toggle("active", side === "sell");
  const btn = $("submitBtn");
  btn.textContent = side === "buy" ? "Submit Buy" : "Submit Sell";
  btn.classList.toggle("sell", side === "sell");
}
$("sideBuy").addEventListener("click", () => setSide("buy"));
$("sideSell").addEventListener("click", () => setSide("sell"));

$("orderForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const body = {
    side: state.side,
    price: parseFloat($("price").value),
    quantity: parseInt($("quantity").value, 10),
  };
  const id = $("orderId").value.trim();
  if (id !== "") body.order_id = parseInt(id, 10);

  log("info", "SUBMIT", `${body.side} ${fmtPrice(body.price)} ×${body.quantity}${id ? " #" + id : ""}`);
  try {
    const { status, data } = await postOrder(body);
    logOrderResult(status, data);
  } catch (err) {
    log("error", "ERROR", String(err));
  }
});

function logOrderResult(status, data) {
  if (!data) {
    log("error", `HTTP ${status}`, "no response body");
    return;
  }
  if (data.status === "accepted") {
    const fills = data.fills && data.fills.length ? ` · ${data.fills.length} fill(s)` : "";
    log("accepted", `HTTP ${status}`, `accepted #${data.order_id} filled ${data.filled} resting ${data.resting}${fills} · ${data.latency_us}µs`);
  } else if (data.status === "rejected") {
    log("rejected", `HTTP ${status}`, `rejected: ${data.reason}${data.field ? " (" + data.field + ")" : ""}`);
  } else if (data.status === "error") {
    log("error", `HTTP ${status}`, data.reason || "engine error");
  } else {
    log("info", `HTTP ${status}`, JSON.stringify(data));
  }
}

/* ---------- cancel ---------- */
$("cancelBtn").addEventListener("click", async () => {
  const id = $("cancelId").value.trim();
  if (id === "") return;
  log("info", "CANCEL", `#${id}`);
  try {
    const { status, data } = await postCancel(id);
    if (data && data.status === "cancelled") {
      log("cancelled", `HTTP ${status}`, `cancelled #${data.order_id} · ${data.latency_us}µs`);
    } else if (data && data.status === "rejected") {
      log("rejected", `HTTP ${status}`, `${data.reason} #${data.order_id ?? id}`);
    } else {
      log("info", `HTTP ${status}`, JSON.stringify(data));
    }
  } catch (err) {
    log("error", "ERROR", String(err));
  }
});

/* ---------- load generator ---------- */
const rnd = (lo, hi) => lo + Math.random() * (hi - lo);
const rndInt = (lo, hi) => Math.floor(rnd(lo, hi + 1));

// Reference price for clustering random orders: last trade, else book mid, else 100.
function refPrice() {
  if (state.lastTradePrice != null) return state.lastTradePrice;
  const b = state.book.bids?.[0]?.price;
  const a = state.book.asks?.[0]?.price;
  if (b != null && a != null) return (a + b) / 2;
  if (b != null) return b;
  if (a != null) return a;
  return 100;
}

function randomOrder() {
  const side = Math.random() < 0.5 ? "buy" : "sell";
  const ticks = rndInt(-4, 4); // +/- a few ticks around reference
  // Buyers lean a touch high, sellers a touch low so they cross and trade.
  const bias = side === "buy" ? 1 : -1;
  const price = Math.max(0.01, refPrice() + (ticks + bias) * 0.25);
  return {
    side,
    price: Math.round(price * 100) / 100,
    quantity: rndInt(1, 10),
  };
}

let loadInFlight = 0;
async function fireOne() {
  loadInFlight++;
  try {
    await postOrder(randomOrder());
  } catch { /* ignore individual failures */ }
  finally { loadInFlight--; }
}

// Burst: fire N orders concurrently (bounded) for a visible spike.
$("burstBtn").addEventListener("click", async () => {
  const n = Math.max(1, Math.min(5000, parseInt($("burstCount").value, 10) || 0));
  const btn = $("burstBtn");
  btn.disabled = true;
  log("info", "BURST", `firing ${n} orders`);
  const t0 = performance.now();
  const CONC = 32;
  let sent = 0;
  async function worker() {
    while (sent < n) {
      sent++;
      await fireOne();
    }
  }
  await Promise.all(Array.from({ length: Math.min(CONC, n) }, worker));
  const dt = ((performance.now() - t0) / 1000).toFixed(2);
  log("info", "BURST", `done ${n} in ${dt}s`);
  if (!state.wsLive) pollOnce();
  btn.disabled = false;
});

// Stream: continuous random orders at the chosen rate.
let streamTimer = null;
function rateInterval() {
  const rate = Math.max(1, parseInt($("rate").value, 10) || 1);
  return 1000 / rate;
}
function startStream() {
  if (streamTimer) return;
  const tick = () => {
    if (loadInFlight < 200) fireOne();
  };
  streamTimer = setInterval(tick, rateInterval());
  const btn = $("streamBtn");
  btn.textContent = "Stop Stream";
  btn.classList.add("active");
  $("loadStat").textContent = `streaming · ${$("rate").value} ord/s`;
  log("info", "STREAM", `started @ ${$("rate").value} ord/s`);
}
function stopStream() {
  if (!streamTimer) return;
  clearInterval(streamTimer);
  streamTimer = null;
  const btn = $("streamBtn");
  btn.textContent = "Start Stream";
  btn.classList.remove("active");
  $("loadStat").textContent = "idle";
  log("info", "STREAM", "stopped");
}
$("streamBtn").addEventListener("click", () => (streamTimer ? stopStream() : startStream()));
$("rate").addEventListener("input", () => {
  $("rateVal").textContent = $("rate").value;
  if (streamTimer) {
    clearInterval(streamTimer);
    streamTimer = setInterval(() => { if (loadInFlight < 200) fireOne(); }, rateInterval());
    $("loadStat").textContent = `streaming · ${$("rate").value} ord/s`;
  }
});

window.addEventListener("beforeunload", () => { stopStream(); stopPolling(); });
window.addEventListener("resize", drawSpark);

/* ---------- boot ---------- */
setSide("buy");
startPolling();   // immediate data; WS will take over and pause polling when it connects
connectWs();
