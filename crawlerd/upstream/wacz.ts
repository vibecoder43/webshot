import type { RunRequest } from "../src/contracts.js";
import type { WarcBuildOutput } from "./warc.js";
import { createStoredZip } from "./zip.js";

export function buildWacz(
  run: RunRequest,
  pagesJsonl: Uint8Array,
  warc: WarcBuildOutput,
  stdoutLog: Uint8Array,
  stderrLog: Uint8Array,
): Uint8Array {
  const cdxj = buildCdxj(warc);
  const waczPagesJsonl = buildWaczPagesJsonl(pagesJsonl);
  const datapackage = Buffer.from(
    JSON.stringify(
      {
        profile: "data-package",
        created: new Date().toISOString(),
        wacz_version: "1.1.1",
        software: "crawlerd rewritten upstream",
        title: run.url,
        resources: [
          {
            name: "archive",
            path: "archive/data.warc",
            bytes: warc.bytes.byteLength,
          },
          {
            name: "pages",
            path: "pages/pages.jsonl",
            bytes: waczPagesJsonl.byteLength,
          },
          {
            name: "stdout log",
            path: "logs/stdout.log",
            bytes: stdoutLog.byteLength,
          },
          {
            name: "stderr log",
            path: "logs/stderr.log",
            bytes: stderrLog.byteLength,
          },
          {
            name: "index",
            path: "indexes/index.cdxj",
            bytes: Buffer.byteLength(cdxj, "utf8"),
          },
        ],
      },
      null,
      2,
    ),
    "utf8",
  );

  return createStoredZip([
    { name: "datapackage.json", body: datapackage },
    { name: "archive/data.warc", body: warc.bytes },
    { name: "pages/pages.jsonl", body: waczPagesJsonl },
    { name: "logs/stdout.log", body: stdoutLog },
    { name: "logs/stderr.log", body: stderrLog },
    {
      name: "indexes/index.cdxj",
      body: Buffer.from(cdxj, "utf8"),
    },
  ]);
}

function buildCdxj(warc: WarcBuildOutput): string {
  return warc.cdxRecords
    .map((record) => `${toSurtKey(record.url)} ${record.timestamp} ${JSON.stringify({
      url: record.url,
      status: `${record.statusCode}`,
      mime: record.headers["content-type"] ?? "application/octet-stream",
      filename: "archive/data.warc",
      length: `${record.length}`,
      offset: `${record.offset}`,
    })}\n`)
    .join("");
}

function toSurtKey(rawUrl: string): string {
  const parsed = new URL(rawUrl);
  if (!isSurtEligibleProtocol(parsed.protocol)) {
    return rawUrl;
  }
  const host = parsed.hostname.toLowerCase().replace(/\.$/, "");
  if (isIpLiteralAddress(host)) {
    return rawUrl;
  }
  const surtHost = host.split(".").reverse().join(",");
  const port = shouldIncludePort(parsed) ? `:${parsed.port}` : "";
  return `${surtHost}${port})${parsed.pathname || "/"}${parsed.search}`;
}

function buildWaczPagesJsonl(pagesJsonl: Uint8Array): Uint8Array {
  const header = Buffer.from(
    `${JSON.stringify({
      format: "json-pages-1.0",
      id: "pages",
      title: "Seed Pages",
      hasText: false,
    })}\n`,
    "utf8",
  );
  return Buffer.concat([header, Buffer.from(pagesJsonl)]);
}

function shouldIncludePort(url: URL): boolean {
  return url.port !== "" &&
    !(url.protocol === "http:" && url.port === "80") &&
    !(url.protocol === "https:" && url.port === "443");
}

function isSurtEligibleProtocol(protocol: string): boolean {
  return protocol === "http:" || protocol === "https:";
}

function isIpv4Address(host: string): boolean {
  return /^\d{1,3}(?:\.\d{1,3}){3}$/.test(host);
}

function isIpLiteralAddress(host: string): boolean {
  return isIpv4Address(host) || host.includes(":");
}
