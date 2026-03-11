import { randomUUID } from "node:crypto";

import type { CapturedExchange } from "./http_capture.js";

export function buildPagesJsonl(exchange: CapturedExchange): Uint8Array {
  return Buffer.from(
    `${JSON.stringify({
      id: randomUUID(),
      url: exchange.finalUrl,
      title: exchange.title ?? extractHtmlTitle(exchange.body),
      loadState: exchange.statusCode >= 200 && exchange.statusCode < 400 ? 2 : 0,
      mime: exchange.headers["content-type"],
      seed: true,
      ts: exchange.timestamp,
      status: exchange.statusCode,
      depth: 0,
    })}\n`,
    "utf8",
  );
}

function extractHtmlTitle(body: Uint8Array): string | undefined {
  const text = Buffer.from(body).toString("utf8");
  const match = text.match(/<title[^>]*>([^<]*)<\/title>/i);
  return match?.[1]?.trim() || undefined;
}
