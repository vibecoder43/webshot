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
            bytes: pagesJsonl.byteLength,
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
    { name: "pages/pages.jsonl", body: pagesJsonl },
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
    .map((record) => `${record.url} ${record.timestamp} ${JSON.stringify({
      url: record.url,
      status: `${record.statusCode}`,
      mime: record.headers["content-type"] ?? "application/octet-stream",
      filename: "archive/data.warc",
      length: `${record.length}`,
      offset: `${record.offset}`,
    })}\n`)
    .join("");
}
