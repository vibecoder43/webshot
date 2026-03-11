import test from "node:test";
import assert from "node:assert/strict";

import type { RunRequest } from "../src/contracts.js";
import type { CapturedExchange } from "../upstream/http_capture.js";
import { buildWarc, type WarcBuildOutput } from "../upstream/warc.js";
import { buildWacz } from "../upstream/wacz.js";

test("buildWarc preserves captured subresource methods in request records", () => {
  const exchange: CapturedExchange = {
    finalUrl: "https://example.test/seed",
    statusCode: 200,
    statusMessage: "OK",
    headers: {
      "content-type": "text/html; charset=utf-8",
    },
    body: Buffer.from("<html><body>seed</body></html>", "utf8"),
    timestamp: "2026-03-10T00:00:00.000Z",
    redirectChain: ["https://example.test/seed"],
    mainDocumentRedirects: [],
    resources: [
      {
        url: "https://example.test/submit?source=page",
        method: "POST",
        statusCode: 201,
        statusMessage: "Created",
        headers: {
          "content-type": "text/plain; charset=utf-8",
        },
        body: Buffer.from("submitted", "utf8"),
        timestamp: "2026-03-10T00:00:01.000Z",
      },
    ],
    title: "Seed",
  };

  const warc = buildWarc(exchange);
  const warcText = Buffer.from(warc.bytes).toString("utf8");

  assert.equal(warc.cdxRecords.length, 2);
  assert.match(warcText, /WARC-Target-URI: https:\/\/example\.test\/submit\?source=page/);
  assert.match(warcText, /POST \/submit\?source=page HTTP\/1\.1/);
});

test("buildWacz uses SURT keys in cdxj output", () => {
  const exchange: CapturedExchange = {
    finalUrl: "https://www.seed.test/",
    statusCode: 200,
    statusMessage: "OK",
    headers: {
      "content-type": "text/html; charset=utf-8",
    },
    body: Buffer.from("<html><head><title>Seed</title></head><body></body></html>", "utf8"),
    timestamp: "2026-03-11T04:41:21.565Z",
    redirectChain: ["https://seed.test/", "https://www.seed.test/"],
    mainDocumentRedirects: [
      {
        url: "https://seed.test/",
        statusCode: 301,
        statusMessage: "Moved Permanently",
        headers: {
          location: "https://www.seed.test/",
          "content-type": "text/html; charset=utf-8",
        },
        timestamp: "2026-03-11T04:41:21.100Z",
      },
    ],
    resources: [],
    title: "Seed",
  };
  const run: RunRequest = {
    url: "https://seed.test/",
    jobTimeoutMs: 30_000,
  };

  const waczText = Buffer.from(buildWacz(
    run,
    Buffer.from("", "utf8"),
    buildWarc(exchange),
    Buffer.from("", "utf8"),
    Buffer.from("", "utf8"),
  )).toString("utf8");

  assert.match(waczText, /test,seed\)\/ 20260311044121 \{"url":"https:\/\/seed\.test\/"/);
  assert.match(waczText, /test,seed,www\)\/ 20260311044121 \{"url":"https:\/\/www\.seed\.test\/"/);
  assert.doesNotMatch(waczText, /https:\/\/www\.seed\.test\/ 20260311044121 \{/);
});

test("buildWacz keeps IP-literal cdxj keys in raw URL form", () => {
  const exchange: CapturedExchange = {
    finalUrl: "http://127.0.0.1:8080/seed",
    statusCode: 200,
    statusMessage: "OK",
    headers: {
      "content-type": "text/html; charset=utf-8",
    },
    body: Buffer.from("<html><head><title>Seed</title></head><body></body></html>", "utf8"),
    timestamp: "2026-03-11T04:41:21.565Z",
    redirectChain: ["http://127.0.0.1:8080/seed"],
    mainDocumentRedirects: [],
    resources: [],
    title: "Seed",
  };
  const run: RunRequest = {
    url: "http://127.0.0.1:8080/seed",
    jobTimeoutMs: 30_000,
  };

  const waczText = Buffer.from(buildWacz(
    run,
    Buffer.from("", "utf8"),
    buildWarc(exchange),
    Buffer.from("", "utf8"),
    Buffer.from("", "utf8"),
  )).toString("utf8");

  assert.match(waczText, /http:\/\/127\.0\.0\.1:8080\/seed 20260311044121 \{"url":"http:\/\/127\.0\.0\.1:8080\/seed"/);
  assert.doesNotMatch(waczText, /127\.0\.0\.1:8080\)\/seed 20260311044121 \{/);
});

test("buildWacz strips a trailing dot before SURT key generation", () => {
  const exchange: CapturedExchange = {
    finalUrl: "https://seed.test./",
    statusCode: 200,
    statusMessage: "OK",
    headers: {
      "content-type": "text/html; charset=utf-8",
    },
    body: Buffer.from("<html><head><title>Seed</title></head><body></body></html>", "utf8"),
    timestamp: "2026-03-11T04:41:21.565Z",
    redirectChain: ["https://seed.test./"],
    mainDocumentRedirects: [],
    resources: [],
    title: "Seed",
  };
  const run: RunRequest = {
    url: "https://seed.test./",
    jobTimeoutMs: 30_000,
  };

  const waczText = Buffer.from(buildWacz(
    run,
    Buffer.from("", "utf8"),
    buildWarc(exchange),
    Buffer.from("", "utf8"),
    Buffer.from("", "utf8"),
  )).toString("utf8");

  assert.match(waczText, /test,seed\)\/ 20260311044121 \{"url":"https:\/\/seed\.test\.\/"/);
  assert.doesNotMatch(waczText, /,test,seed\)\/ 20260311044121 \{/);
});

test("buildWacz keeps non-http cdxj keys in raw URL form", () => {
  const run: RunRequest = {
    url: "https://seed.test/",
    jobTimeoutMs: 30_000,
  };
  const warc: WarcBuildOutput = {
    bytes: Buffer.from("", "utf8"),
    cdxRecords: [
      {
        url: "blob:https://seed.test/01234567-89ab-cdef-0123-456789abcdef",
        timestamp: "20260311044121",
        statusCode: 200,
        headers: {
          "content-type": "application/octet-stream",
        },
        offset: 0,
        length: 123,
      },
    ],
  };

  const waczText = Buffer.from(buildWacz(
    run,
    Buffer.from("", "utf8"),
    warc,
    Buffer.from("", "utf8"),
    Buffer.from("", "utf8"),
  )).toString("utf8");

  assert.match(waczText, /blob:https:\/\/seed\.test\/01234567-89ab-cdef-0123-456789abcdef 20260311044121 \{"url":"blob:https:\/\/seed\.test\/01234567-89ab-cdef-0123-456789abcdef"/);
  assert.doesNotMatch(waczText, /\)https:\/\/seed\.test\/01234567-89ab-cdef-0123-456789abcdef 20260311044121 \{/);
});
