/** Base error for all client SDK failures. */
export class HmeError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "HmeError";
  }
}

/** A request could not be completed at the transport layer (network, CORS, timeout). */
export class HmeTransportError extends HmeError {
  constructor(message: string) {
    super(message);
    this.name = "HmeTransportError";
  }
}

/** The gateway returned a body the SDK could not interpret as JSON. */
export class HmeProtocolError extends HmeError {
  constructor(message: string) {
    super(message);
    this.name = "HmeProtocolError";
  }
}

/** The gateway rejected the request with HTTP 401 (auth enabled, key missing/invalid). */
export class HmeAuthError extends HmeError {
  readonly status = 401;
  constructor(message = "unauthorized") {
    super(message);
    this.name = "HmeAuthError";
  }
}
