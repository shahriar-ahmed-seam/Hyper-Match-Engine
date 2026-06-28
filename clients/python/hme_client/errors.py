"""Exception types raised by the Hyper-Match-Engine client SDK."""

from __future__ import annotations

from typing import Optional


class HmeError(Exception):
    """Base error for all client SDK failures.

    Raised for transport-level problems (connection refused, timeouts, invalid
    responses). Business-level rejections from the gateway (for example a
    duplicate order id or an unknown order) are reported through the typed
    result objects returned by :class:`hme_client.HmeClient`, not as exceptions.
    """


class HmeTransportError(HmeError):
    """The request could not be completed at the transport layer.

    Examples include connection refusal, DNS failures, and read timeouts.
    """


class HmeProtocolError(HmeError):
    """The gateway returned a response the SDK could not interpret.

    Typically raised when a response body is not valid JSON or is missing
    fields the SDK requires.
    """


class HmeAuthError(HmeError):
    """The gateway rejected the request with ``401 unauthorized``.

    Raised when the gateway has API-key authentication enabled and the request
    was sent without a key, or with an invalid one.
    """

    def __init__(self, message: str = "unauthorized", *, status: Optional[int] = 401) -> None:
        super().__init__(message)
        self.status = status
