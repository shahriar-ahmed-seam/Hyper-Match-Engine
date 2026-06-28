"""Quickstart for the Hyper-Match-Engine Python client.

Run against a locally running gateway::

    python examples/quickstart.py

The script places a resting sell, crosses it with a buy, prints the fills, the
top of book and stats, then (when ``websocket-client`` is installed) streams a
handful of live events. Set ``HME_BASE_URL`` and ``HME_API_KEY`` to override the
defaults.
"""

from __future__ import annotations

import os

from hme_client import HmeClient, HmeError


def main() -> None:
    base_url = os.environ.get("HME_BASE_URL", "http://127.0.0.1:8080")
    api_key = os.environ.get("HME_API_KEY") or None

    client = HmeClient(base_url, api_key=api_key)

    # Place a resting sell, then cross it with a marketable buy.
    resting = client.sell(price=101.00, quantity=10)
    print(f"resting sell -> accepted={resting.accepted} order_id={resting.order_id} "
          f"resting={resting.resting}")

    crossing = client.buy(price=101.00, quantity=4)
    print(f"crossing buy -> accepted={crossing.accepted} filled={crossing.filled} "
          f"resting={crossing.resting}")
    for fill in crossing.fills:
        print(f"  fill qty={fill.quantity} @ {fill.price} "
              f"(resting_id={fill.resting_id}, exec_seq={fill.exec_seq})")

    # Cancel whatever remains of the resting sell.
    if resting.accepted and resting.order_id is not None:
        cancelled = client.cancel(resting.order_id)
        print(f"cancel {resting.order_id} -> cancelled={cancelled.cancelled} "
              f"reason={cancelled.reason}")

    # Top of book and a stats snapshot.
    book = client.book(depth=5)
    best_bid = book["bids"][0] if book.get("bids") else None
    best_ask = book["asks"][0] if book.get("asks") else None
    print(f"top of book -> bid={best_bid} ask={best_ask} sequence={book.get('sequence')}")

    stats = client.stats()
    print(f"stats -> accepted={stats.get('orders_accepted')} "
          f"trades={stats.get('trades')} resting={stats.get('resting_orders')} "
          f"latency_us={stats.get('latency_us')}")

    stream_events(base_url, api_key)
    client.close()


def stream_events(base_url: str, api_key: str | None) -> None:
    """Stream ~10 events if the optional streaming dependency is available."""
    try:
        from hme_client import events
    except HmeError:
        print("streaming skipped (install with: pip install \"hme-client[stream]\")")
        return

    print("streaming up to 10 events ...")
    try:
        count = 0
        for event in events(base_url, api_key=api_key, reconnect=False):
            if event.get("type") in {"trade", "accepted"}:
                print(f"  event: {event}")
                count += 1
                if count >= 10:
                    break
    except HmeError as exc:
        print(f"streaming unavailable: {exc}")


if __name__ == "__main__":
    main()
