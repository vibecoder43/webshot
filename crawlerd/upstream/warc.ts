import { randomUUID } from "node:crypto";

import type { CapturedExchange } from "./http_capture.js";

export type WarcCdxRecord = {
  url: string;
  timestamp: string;
  statusCode: number;
  headers: Record<string, string>;
  offset: number;
  length: number;
};

export type WarcBuildOutput = {
  bytes: Uint8Array;
  cdxRecords: WarcCdxRecord[];
};

type SerializableResponse = {
  url: string;
  statusCode: number;
  statusMessage: string;
  headers: Record<string, string>;
  body: Uint8Array;
  timestamp: string;
};

const kUserAgent = "crawlerd/0.1.0";

export function buildWarc(exchange: CapturedExchange): WarcBuildOutput {
  const responses = [
    ...exchange.mainDocumentRedirects.map((redirect) => ({
      ...redirect,
      body: new Uint8Array(),
    })),
    {
      url: exchange.finalUrl,
      statusCode: exchange.statusCode,
      statusMessage: exchange.statusMessage,
      headers: exchange.headers,
      body: exchange.body,
      timestamp: exchange.timestamp,
    },
  ];

  const chunks: Buffer[] = [];
  const cdxRecords: WarcCdxRecord[] = [];
  let offset = 0;

  for (const response of responses) {
    const serialized = serializeRecordPair(response);
    chunks.push(serialized.responseBytes, serialized.requestBytes);
    cdxRecords.push({
      url: response.url,
      timestamp: toCdxTimestamp(response.timestamp),
      statusCode: response.statusCode,
      headers: response.headers,
      offset,
      length: serialized.responseBytes.byteLength,
    });
    offset += serialized.responseBytes.byteLength + serialized.requestBytes.byteLength;
  }

  return {
    bytes: Buffer.concat(chunks),
    cdxRecords,
  };
}

function serializeRecordPair(response: SerializableResponse): {
  responseBytes: Buffer;
  requestBytes: Buffer;
} {
  const recordDate = response.timestamp;
  const responseRecordId = `urn:uuid:${randomUUID()}`;
  const requestRecordId = `urn:uuid:${randomUUID()}`;
  const normalizedHeaders = normalizeResponseHeaders(response.headers);
  const statusMessage = response.statusMessage || defaultStatusMessage(response.statusCode);

  const httpResponseHead = [
    `HTTP/1.1 ${response.statusCode} ${statusMessage}`,
    ...Object.entries(normalizedHeaders).map(([name, value]) => `${name}: ${value}`),
    "",
    "",
  ].join("\r\n");

  const responseHeader = [
    "WARC/1.1",
    "WARC-Type: response",
    `WARC-Target-URI: ${response.url}`,
    `WARC-Date: ${recordDate}`,
    `WARC-Record-ID: <${responseRecordId}>`,
    "Content-Type: application/http; msgtype=response",
    `Content-Length: ${Buffer.byteLength(httpResponseHead) + response.body.byteLength}`,
    "",
    httpResponseHead,
  ].join("\r\n");

  const requestPayload = [
    `GET ${requestPathFromUrl(response.url)} HTTP/1.1`,
    `Host: ${new URL(response.url).host}`,
    `User-Agent: ${kUserAgent}`,
    "",
    "",
  ].join("\r\n");

  const requestHeader = [
    "WARC/1.1",
    "WARC-Type: request",
    `WARC-Target-URI: ${response.url}`,
    `WARC-Date: ${recordDate}`,
    `WARC-Record-ID: <${requestRecordId}>`,
    `WARC-Concurrent-To: <${responseRecordId}>`,
    "Content-Type: application/http; msgtype=request",
    `Content-Length: ${Buffer.byteLength(requestPayload)}`,
    "",
    requestPayload,
    "\r\n",
  ].join("\r\n");

  return {
    responseBytes: Buffer.concat([
      Buffer.from(responseHeader, "utf8"),
      Buffer.from(response.body),
      Buffer.from("\r\n\r\n", "utf8"),
    ]),
    requestBytes: Buffer.from(requestHeader, "utf8"),
  };
}

function normalizeResponseHeaders(headers: Record<string, string>): Record<string, string> {
  return Object.fromEntries(
    Object.entries(headers).filter(([name]) => {
      const lower = name.toLowerCase();
      return lower !== "content-encoding" &&
        lower !== "content-length" &&
        lower !== "transfer-encoding";
    }),
  );
}

function requestPathFromUrl(rawUrl: string): string {
  const parsed = new URL(rawUrl);
  return `${parsed.pathname || "/"}${parsed.search}`;
}

function toCdxTimestamp(iso: string): string {
  return iso.replace(/[-:TZ.]/g, "").slice(0, 14);
}

function defaultStatusMessage(statusCode: number): string {
  switch (statusCode) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 303:
      return "See Other";
    case 307:
      return "Temporary Redirect";
    case 308:
      return "Permanent Redirect";
    case 404:
      return "Not Found";
    case 500:
      return "Internal Server Error";
    default:
      return "";
  }
}
