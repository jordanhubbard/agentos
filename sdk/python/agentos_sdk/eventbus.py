"""
Async EventBus client — subscribe to and publish on agentOS named channels.

Maps to agentOS MsgBus operations:
    MSGBUS_OP_SUBSCRIBE  (0x102)
    MSGBUS_OP_PUBLISH    (0x104)

Wire protocol
─────────────
The client maintains a persistent Unix-socket connection to the shim and
multiplexes subscriptions over a single stream.  Outbound frames are
newline-delimited JSON; inbound frames are the same.

Reconnection
────────────
On connection loss the client re-subscribes all active channels using
exponential back-off capped at *max_backoff_s*.  Callbacks are delivered
exactly once per event; duplicate suppression uses the event ``"id"`` field
when present.
"""

from __future__ import annotations

import asyncio
import json
import logging
import uuid
from collections import defaultdict
from typing import Any, Callable

log = logging.getLogger(__name__)

Callback = Callable[[dict[str, Any]], Any]


class EventBus:
    """
    Async publish/subscribe client for agentOS named channels.

    Parameters
    ----------
    socket_path:
        Unix socket path of the agentOS shim.
    mock:
        When ``True`` the bus operates in local mock mode — subscribers receive
        events published by the same process only.
    max_backoff_s:
        Maximum reconnect back-off interval in seconds.
    """

    def __init__(
        self,
        socket_path: str = "/tmp/agentos.sock",
        *,
        mock: bool = False,
        max_backoff_s: float = 30.0,
    ) -> None:
        self._socket_path = socket_path
        self._mock = mock
        self._max_backoff = max_backoff_s

        # channel → list of callbacks
        self._subscriptions: dict[str, list[Callback]] = defaultdict(list)
        # seen event IDs for dedup
        self._seen: set[str] = set()

        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._recv_task: asyncio.Task[None] | None = None
        self._connected = asyncio.Event()

    # ── public API ─────────────────────────────────────────────────────────────

    async def connect(self) -> None:
        """Open the connection and start the receive loop."""
        if self._mock:
            self._connected.set()
            return
        await self._do_connect()

    async def close(self) -> None:
        """Disconnect and cancel background tasks."""
        if self._recv_task:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except asyncio.CancelledError:
                pass
        if self._writer:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except Exception:
                pass
        self._connected.clear()

    async def subscribe(self, channel: str, callback: Callback) -> None:
        """
        Subscribe *callback* to *channel*.

        The callback is invoked with a single ``dict`` argument whenever an
        event is delivered on that channel.  Multiple callbacks may be
        registered for the same channel.

        Parameters
        ----------
        channel:
            Named channel, e.g. ``"system.events"`` or ``"tools.registry"``.
        callback:
            Async or sync callable accepting one ``dict`` argument.
        """
        self._subscriptions[channel].append(callback)

        if not self._mock:
            await self._send_frame({"op": "msgbus.subscribe", "channel": channel})

        log.debug("Subscribed to channel %r", channel)

    async def unsubscribe(self, channel: str, callback: Callback | None = None) -> None:
        """
        Remove a subscription.

        Parameters
        ----------
        channel:
            Channel name to unsubscribe from.
        callback:
            Specific callback to remove.  When ``None`` all callbacks for the
            channel are removed and the channel subscription is cancelled.
        """
        if callback is None:
            self._subscriptions.pop(channel, None)
            if not self._mock:
                await self._send_frame({"op": "msgbus.unsubscribe", "channel": channel})
        else:
            subs = self._subscriptions.get(channel, [])
            try:
                subs.remove(callback)
            except ValueError:
                pass
            if not subs:
                self._subscriptions.pop(channel, None)
                if not self._mock:
                    await self._send_frame({"op": "msgbus.unsubscribe", "channel": channel})

    async def publish(self, channel: str, event: dict[str, Any]) -> None:
        """
        Publish *event* to *channel*.

        In mock mode the event is delivered synchronously to local subscribers
        without touching the socket.

        Parameters
        ----------
        channel:
            Target channel name.
        event:
            Arbitrary JSON-serialisable payload dict.  An ``"id"`` field will
            be added automatically if not present.
        """
        event.setdefault("id", str(uuid.uuid4()))

        if self._mock:
            await self._dispatch(channel, event)
            return

        await self._send_frame({
            "op": "msgbus.publish",
            "channel": channel,
            "payload": event,
        })

    async def wait_for(
        self,
        channel: str,
        predicate: Callable[[dict[str, Any]], bool] | None = None,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        """
        Wait for a single event on *channel* matching an optional *predicate*.

        Parameters
        ----------
        channel:
            Channel to listen on.
        predicate:
            Optional filter function; when supplied only events for which the
            predicate returns ``True`` are returned.
        timeout:
            Maximum seconds to wait; raises :exc:`asyncio.TimeoutError` on
            expiry.

        Returns
        -------
        dict
            The first matching event payload.
        """
        fut: asyncio.Future[dict[str, Any]] = asyncio.get_event_loop().create_future()

        async def _cb(event: dict[str, Any]) -> None:
            if fut.done():
                return
            if predicate is None or predicate(event):
                fut.set_result(event)
                await self.unsubscribe(channel, _cb)  # type: ignore[arg-type]

        await self.subscribe(channel, _cb)  # type: ignore[arg-type]
        try:
            return await asyncio.wait_for(fut, timeout=timeout)
        except asyncio.TimeoutError:
            await self.unsubscribe(channel, _cb)  # type: ignore[arg-type]
            raise

    # ── internals ──────────────────────────────────────────────────────────────

    async def _do_connect(self, backoff: float = 1.0) -> None:
        import os

        if not os.path.exists(self._socket_path):
            log.info("EventBus: socket not found at %s — using mock mode", self._socket_path)
            self._mock = True
            self._connected.set()
            return

        while True:
            try:
                self._reader, self._writer = await asyncio.open_unix_connection(
                    self._socket_path
                )
                self._connected.set()
                log.info("EventBus connected to %s", self._socket_path)
                # Re-subscribe any existing channels (reconnect scenario)
                for channel in list(self._subscriptions):
                    await self._send_frame({"op": "msgbus.subscribe", "channel": channel})
                self._recv_task = asyncio.get_event_loop().create_task(self._recv_loop())
                return
            except OSError as exc:
                log.warning("EventBus connect failed: %s — retry in %.1fs", exc, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, self._max_backoff)

    async def _recv_loop(self) -> None:
        assert self._reader is not None
        try:
            while True:
                line = await self._reader.readline()
                if not line:
                    break
                try:
                    frame: dict[str, Any] = json.loads(line)
                except json.JSONDecodeError:
                    log.warning("EventBus: malformed frame ignored")
                    continue

                channel = frame.get("channel", "")
                payload = frame.get("payload", frame)
                await self._dispatch(channel, payload)
        except asyncio.CancelledError:
            pass
        except Exception as exc:
            log.warning("EventBus receive loop error: %s — reconnecting", exc)
            self._connected.clear()
            await self._do_connect()

    async def _dispatch(self, channel: str, event: dict[str, Any]) -> None:
        """Deliver *event* to all callbacks registered for *channel*."""
        event_id = event.get("id", "")
        if event_id and event_id in self._seen:
            return  # Deduplicate
        if event_id:
            self._seen.add(event_id)
            # Bound the seen set size
            if len(self._seen) > 10_000:
                self._seen.clear()

        callbacks = list(self._subscriptions.get(channel, []))
        for cb in callbacks:
            try:
                result = cb(event)
                if asyncio.iscoroutine(result):
                    await result
            except Exception:
                log.exception("EventBus callback error on channel %r", channel)

    async def _send_frame(self, data: dict[str, Any]) -> None:
        if self._writer is None:
            return
        try:
            self._writer.write((json.dumps(data) + "\n").encode())
            await self._writer.drain()
        except OSError as exc:
            log.warning("EventBus send failed: %s", exc)

    # ── context manager ────────────────────────────────────────────────────────

    async def __aenter__(self) -> "EventBus":
        await self.connect()
        return self

    async def __aexit__(self, *_: Any) -> None:
        await self.close()
