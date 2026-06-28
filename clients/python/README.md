# hme-client

A small, professional Python client SDK for the Hyper-Match-Engine gateway's
public HTTP + WebSocket API. It talks to a running gateway over HTTP/WS and has
no dependency on the engine's Rust/C++ code.

## Install

```bash
# HTTP client only
pip install -e .

# HTTP client + WebSocket streaming
pip install -e ".[stream]"
```

Requires Python 3.9+. The HTTP client depends on `requests`; streaming adds
`websocket-client` and is imported lazily, so the SDK imports fine without it.

## Usage

```python
from hme_client import HmeClient, events

client = HmeClient("http://127.0.0.1:8080")        # add api_key=... if required
resting = client.sell(price=101.0, quantity=10)    # OrderAck
crossing = client.buy(price=101.0, quantity=4)     # crosses the resting sell
print(crossing.accepted, crossing.filled, [f.price for f in crossing.fills])
print(client.cancel(resting.order_id).cancelled)   # CancelResult
print(client.book(depth=5)["asks"][:1], client.stats()["trades"])

for event in events("http://127.0.0.1:8080", reconnect=False):  # WebSocket stream
    print(event["type"], event)
```

## Result conventions

`submit_order` (and the `buy`/`sell` wrappers) always return an `OrderAck`
rather than raising on a business rejection. Branch on `ack.accepted`:

- `ack.accepted is True` -> use `ack.order_id`, `ack.filled`, `ack.resting`,
  and `ack.fills`.
- `ack.accepted is False` -> read `ack.reason` (for example
  `"duplicate_order_id"`, `"engine_unavailable"`) and `ack.field_name`.

`ack.http_status` and `ack.raw` expose the underlying status code and JSON.
`cancel` returns a `CancelResult` with `.cancelled` and `.reason`. Only
transport failures, unparseable responses, and `401` raise exceptions
(`HmeTransportError`, `HmeProtocolError`, `HmeAuthError`; all subclass
`HmeError`).

## Authentication

When the gateway runs with API keys, mutating calls require an `X-API-Key`
header. Pass `api_key=` and the SDK sends it on every HTTP request and on the
WebSocket connection:

```python
client = HmeClient("http://127.0.0.1:8080", api_key="your-key")
```

## Example

`examples/quickstart.py` is runnable against a local gateway:

```bash
python examples/quickstart.py
```
