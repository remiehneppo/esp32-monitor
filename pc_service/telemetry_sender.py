"""TelemetrySender — validates a target address and POSTs a binary telemetry payload."""

from urllib.parse import urlsplit
from urllib.request import Request, urlopen


class TelemetrySender:
    """Posts a packed binary telemetry payload to a single ESP32 device.

    Seam: the ``send`` method is the entire interface. Callers (telemetry loop)
    are decoupled from URL construction, validation, and HTTP semantics.
    The class can be replaced with a mock in tests without touching the loop.
    """

    TELEMETRY_PATH = "/api/telemetry"
    TIMEOUT = 1.0

    def send(self, address: str, payload: bytes) -> None:
        """POST *payload* to the telemetry endpoint on *address*.

        Args:
            address: ``host`` or ``host:port`` string (IPv4, no scheme).
            payload: Packed binary telemetry (see ``protocol.pack_telemetry``).

        Raises:
            ValueError:  *address* resolves to an invalid or disallowed URL.
            OSError:     Network or HTTP error from ``urlopen``.
        """
        url = f"http://{address}{self.TELEMETRY_PATH}"
        parsed = urlsplit(url)
        if parsed.scheme != "http" or parsed.path != self.TELEMETRY_PATH or parsed.hostname is None:
            raise ValueError(f"Invalid telemetry target: {address!r}")

        request = Request(
            url,
            data=payload,
            method="POST",
            headers={"Content-Type": "application/octet-stream"},
        )
        with urlopen(request, timeout=self.TIMEOUT) as response:
            response.read()
