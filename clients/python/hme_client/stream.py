"""WebSocket event streaming for the Hyper-Match-Engine gateway.

This module depends on the optional ``websocket-client`` package (import name
``websocket``). It is imported lazily so the HTTP client keeps working when the
streaming extra is not installed. Install it with::

    pip install "hme-client[stream]"
"""

from __future__ import annotations

import json
import time
from typing import Any, Callable, Dict, Iterator, Optional

from .errors import HmeError

__all__ = ["events", "stream"]

_RECONNECT_DELAY_SECONDS = 1.0


def _ws_url(base_url: str) -> str:
    """Derive the ``/ws`` endpoint URL from an HTTP base URL."""
    url = base_url.rstrip("/")
    if url.startswith("https://"):
        url = "wss://" + url[len("https://") :]
    elif url.startswith("http://"):
        url = "ws://" + url[len("http://") :]
    return f"{url}/ws"


def _import_websocket() -> Any:
    try:
        import websocket  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised without the extra
        raise HmeError(
            "WebSocket streaming requires the 'websocket-client' package. "
            "Install it with: pip install \"hme-client[stream]\""
        ) from exc
    return websocket


def events(
    base_url: str = "http://127.0.0.1:8080",
    *,
    api_key: Optional[str] = None,
    reconnect: bool = True,
    timeout: float = 5.0,
) -> Iterator[Dict[str, Any]]:
    """Yield parsed event dicts from the gateway's ``/ws`` endpoint.

    Each yielded value is a decoded JSON frame whose ``type`` is one of
    ``trade``, ``accepted``, ``rejected``, ``cancelled``, ``book``, or
    ``stats``. Non-JSON frames are skipped.

    Args:
        base_url: HTTP base URL of the gateway; the scheme is mapped to
            ``ws``/``wss`` and ``/ws`` is appended automatically.
        api_key: Optional API key sent as the ``X-API-Key`` header.
        reconnect: When ``True`` (default), transparently reconnect after a
            dropped connection, waiting briefly between attempts. When
            ``False``, the generator returns once the connection closes.
        timeout: Socket connect/read timeout in seconds.

    Yields:
        Parsed event dictionaries.

    Raises:
        HmeError: The ``websocket-client`` package is not installed.
    """
    websocket = _import_websocket()
    url = _ws_url(base_url)
    header = [f"X-API-Key: {api_key}"] if api_key else None

    while True:
        ws = None
        try:
            ws = websocket.create_connection(url, header=header, timeout=timeout)
            while True:
                frame = ws.recv()
                if frame is None or frame == "":
                    break
                if isinstance(frame, bytes):
                    frame = frame.decode("utf-8", "replace")
                try:
                    event = json.loads(frame)
                except ValueError:
                    continue
                if isinstance(event, dict):
                    yield event
        except Exception as exc:  # noqa: BLE001 - normalize transport failures
            if not reconnect:
                raise HmeError(f"WebSocket stream failed: {exc}") from exc
        finally:
            if ws is not None:
                try:
                    ws.close()
                except Exception:  # noqa: BLE001 - best-effort cleanup
                    pass

        if not reconnect:
            return
        time.sleep(_RECONNECT_DELAY_SECONDS)


def stream(
    base_url: str = "http://127.0.0.1:8080",
    *,
    on_event: Callable[[Dict[str, Any]], Any],
    api_key: Optional[str] = None,
    reconnect: bool = True,
    timeout: float = 5.0,
) -> None:
    """Stream events and invoke ``on_event`` for each one.

    A blocking convenience wrapper around :func:`events`. The call runs until
    the connection closes (when ``reconnect`` is ``False``), the callback
    raises, or the caller interrupts it.

    Args:
        base_url: HTTP base URL of the gateway.
        on_event: Callable invoked with each parsed event dict.
        api_key: Optional API key sent as the ``X-API-Key`` header.
        reconnect: Whether to reconnect after a dropped connection.
        timeout: Socket connect/read timeout in seconds.
    """
    for event in events(
        base_url, api_key=api_key, reconnect=reconnect, timeout=timeout
    ):
        on_event(event)
