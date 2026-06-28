"""Synchronous HTTP client for the Hyper-Match-Engine gateway."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

import requests

from .errors import HmeAuthError, HmeProtocolError, HmeTransportError

__all__ = ["HmeClient", "Fill", "OrderAck", "CancelResult"]


@dataclass(frozen=True)
class Fill:
    """A single execution produced when an incoming order crosses the book."""

    price: float
    quantity: int
    resting_id: int
    exec_seq: int

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "Fill":
        return cls(
            price=data["price"],
            quantity=data["quantity"],
            resting_id=data["resting_id"],
            exec_seq=data["exec_seq"],
        )


@dataclass(frozen=True)
class OrderAck:
    """Outcome of a :meth:`HmeClient.submit_order` call.

    The gateway answers every well-formed submission, whether it is accepted or
    rejected, so this object always carries the parsed response. Inspect
    :attr:`accepted` to branch:

    * ``accepted is True`` -> the order was processed; :attr:`fills`,
      :attr:`filled`, and :attr:`resting` describe the result.
    * ``accepted is False`` -> the order was rejected; :attr:`reason` explains
      why (for example ``"duplicate_order_id"`` or ``"engine_unavailable"``)
      and :attr:`field` names the offending field when the gateway provides it.

    :attr:`http_status` holds the raw HTTP status code and :attr:`raw` holds the
    unmodified JSON body for callers that need fields not surfaced here.
    """

    accepted: bool
    http_status: int
    raw: Dict[str, Any]
    order_id: Optional[int] = None
    side: Optional[str] = None
    price: Optional[float] = None
    quantity: Optional[int] = None
    filled: int = 0
    resting: int = 0
    fills: List[Fill] = field(default_factory=list)
    latency_us: Optional[float] = None
    reason: Optional[str] = None
    field_name: Optional[str] = None

    @property
    def rejected(self) -> bool:
        """``True`` when the gateway did not accept the order."""
        return not self.accepted

    @classmethod
    def from_response(cls, status: int, body: Dict[str, Any]) -> "OrderAck":
        accepted = body.get("status") == "accepted"
        fills = [Fill.from_dict(f) for f in body.get("fills", [])]
        return cls(
            accepted=accepted,
            http_status=status,
            raw=body,
            order_id=body.get("order_id"),
            side=body.get("side"),
            price=body.get("price"),
            quantity=body.get("quantity"),
            filled=body.get("filled", 0),
            resting=body.get("resting", 0),
            fills=fills,
            latency_us=body.get("latency_us"),
            reason=body.get("reason"),
            field_name=body.get("field"),
        )


@dataclass(frozen=True)
class CancelResult:
    """Outcome of a :meth:`HmeClient.cancel` call.

    ``cancelled is True`` when the resting order was removed. Otherwise the order
    could not be cancelled and :attr:`reason` is one of ``"order_not_found"`` or
    ``"no_longer_resting"``.
    """

    cancelled: bool
    http_status: int
    raw: Dict[str, Any]
    order_id: Optional[int] = None
    latency_us: Optional[float] = None
    reason: Optional[str] = None

    @classmethod
    def from_response(cls, status: int, body: Dict[str, Any]) -> "CancelResult":
        return cls(
            cancelled=body.get("status") == "cancelled",
            http_status=status,
            raw=body,
            order_id=body.get("order_id"),
            latency_us=body.get("latency_us"),
            reason=body.get("reason"),
        )


class HmeClient:
    """Client for the Hyper-Match-Engine gateway's public HTTP API.

    The client wraps the gateway's REST endpoints with a small, typed surface.
    A single :class:`requests.Session` is reused across calls for connection
    pooling. When ``api_key`` is supplied it is sent as the ``X-API-Key`` header
    on every request, which mutating endpoints require when the gateway runs
    with authentication enabled.

    Args:
        base_url: Root URL of the gateway. Trailing slashes are trimmed.
        api_key: Optional API key forwarded as ``X-API-Key``.
        timeout: Per-request timeout in seconds.

    Example:
        >>> client = HmeClient("http://127.0.0.1:8080", api_key="secret")
        >>> ack = client.buy(price=101.5, quantity=10)
        >>> if ack.accepted:
        ...     print(ack.order_id, ack.filled, ack.resting)
    """

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8080",
        api_key: Optional[str] = None,
        timeout: float = 5.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout
        self._session = requests.Session()
        if api_key:
            self._session.headers["X-API-Key"] = api_key

    def __enter__(self) -> "HmeClient":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def close(self) -> None:
        """Close the underlying HTTP session."""
        self._session.close()

    def submit_order(
        self,
        side: str,
        price: float,
        quantity: int,
        order_id: Optional[int] = None,
    ) -> OrderAck:
        """Submit a new order.

        Args:
            side: ``"buy"`` or ``"sell"``.
            price: Limit price.
            quantity: Order size in units; must be positive.
            order_id: Optional client-assigned id. When omitted the gateway
                assigns one and returns it on the :class:`OrderAck`.

        Returns:
            An :class:`OrderAck`. Check :attr:`OrderAck.accepted` to determine
            whether the order was processed or rejected.

        Raises:
            HmeAuthError: The gateway returned ``401 unauthorized``.
            HmeTransportError: The request could not be completed.
            HmeProtocolError: The response body was not valid JSON.
        """
        payload: Dict[str, Any] = {
            "side": side,
            "price": price,
            "quantity": quantity,
        }
        if order_id is not None:
            payload["order_id"] = order_id
        status, body = self._request("POST", "/api/orders", json=payload)
        return OrderAck.from_response(status, body)

    def buy(self, price: float, quantity: int, **kwargs: Any) -> OrderAck:
        """Submit a buy order. Convenience wrapper over :meth:`submit_order`."""
        return self.submit_order("buy", price, quantity, **kwargs)

    def sell(self, price: float, quantity: int, **kwargs: Any) -> OrderAck:
        """Submit a sell order. Convenience wrapper over :meth:`submit_order`."""
        return self.submit_order("sell", price, quantity, **kwargs)

    def cancel(self, order_id: int) -> CancelResult:
        """Cancel a resting order by id.

        Returns:
            A :class:`CancelResult`. Check :attr:`CancelResult.cancelled`.

        Raises:
            HmeAuthError: The gateway returned ``401 unauthorized``.
            HmeTransportError: The request could not be completed.
            HmeProtocolError: The response body was not valid JSON.
        """
        status, body = self._request("POST", f"/api/cancel/{order_id}")
        return CancelResult.from_response(status, body)

    def book(self, depth: int = 15) -> Dict[str, Any]:
        """Return the current order book to ``depth`` levels per side.

        The response contains ``bids``, ``asks``, and a ``sequence`` number.
        """
        _status, body = self._request("GET", "/api/book", params={"depth": depth})
        return body

    def stats(self) -> Dict[str, Any]:
        """Return gateway and engine statistics as a dict."""
        _status, body = self._request("GET", "/api/stats")
        return body

    def trades(self, limit: int = 50) -> List[Dict[str, Any]]:
        """Return up to ``limit`` of the most recent trades."""
        _status, body = self._request("GET", "/api/trades", params={"limit": limit})
        return body

    def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[Dict[str, Any]] = None,
        params: Optional[Dict[str, Any]] = None,
    ) -> Any:
        url = f"{self.base_url}{path}"
        try:
            response = self._session.request(
                method,
                url,
                json=json,
                params=params,
                timeout=self.timeout,
            )
        except requests.RequestException as exc:
            raise HmeTransportError(f"request to {url} failed: {exc}") from exc

        if response.status_code == 401:
            raise HmeAuthError(status=401)

        try:
            body = response.json()
        except ValueError as exc:
            raise HmeProtocolError(
                f"non-JSON response from {url} (status {response.status_code})"
            ) from exc

        return response.status_code, body
