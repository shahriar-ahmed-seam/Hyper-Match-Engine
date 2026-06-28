"""Python client SDK for the Hyper-Match-Engine gateway.

Public API:

* :class:`HmeClient` - synchronous HTTP client for orders, cancels, book,
  stats, and trades.
* :class:`OrderAck`, :class:`CancelResult`, :class:`Fill` - typed result
  objects.
* :func:`events` / :func:`stream` - WebSocket event streaming (requires the
  optional ``stream`` extra).
* :class:`HmeError` and subclasses - error types.
"""

from __future__ import annotations

from .client import CancelResult, Fill, HmeClient, OrderAck
from .errors import HmeAuthError, HmeError, HmeProtocolError, HmeTransportError
from .stream import events, stream

__all__ = [
    "HmeClient",
    "OrderAck",
    "CancelResult",
    "Fill",
    "events",
    "stream",
    "HmeError",
    "HmeTransportError",
    "HmeProtocolError",
    "HmeAuthError",
]

__version__ = "0.1.0"
