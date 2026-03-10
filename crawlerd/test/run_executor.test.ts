import test from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import { once } from "node:events";

import { kBrowserBin, kBrowserGeometry } from "../src/config.js";
import { UpstreamRunExecutor } from "../src/run_executor.js";
import type { RunRequest } from "../src/contracts.js";

test("UpstreamRunExecutor captures through Chromium and returns an in-memory WACZ", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    assert.equal(request.url, "/seed");
    assert.equal(request.headers["x-webshot-via-proxy"], "1");
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end("<html><head><title>Seed Title</title></head><body>Hello</body></html>");
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);

  const proxyServer = createProxyServer();
  proxyServer.listen(0, "127.0.0.1");
  await once(proxyServer, "listening");
  const proxyPort = getListenPort(proxyServer);

  const executor = createExecutor(`http://127.0.0.1:${proxyPort}`);

  try {
    const result = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/seed`,
    }));

    assert.equal(result.exitCode, 0);
    assert.deepEqual(result.seedProbe, {
      status: 200,
      load_state: 2,
    });
    assert.match(Buffer.from(result.artifacts.pages ?? []).toString("utf8"), /"title":"Seed Title"/);
    assert.match(Buffer.from(result.artifacts.stdoutLog).toString("utf8"), /browser_pid=\d+/);
    assert.match(Buffer.from(result.artifacts.stdoutLog).toString("utf8"), /reused_browser=false/);
    assert.equal(Buffer.from(result.artifacts.stderrLog).toString("utf8"), "");

    const zipEntries = parseStoredZip(Buffer.from(result.artifacts.wacz ?? []));
    assert.deepEqual(
      Object.keys(zipEntries).sort(),
      [
        "archive/data.warc",
        "datapackage.json",
        "indexes/index.cdxj",
        "logs/stderr.log",
        "logs/stdout.log",
        "pages/pages.jsonl",
      ],
    );
    assert.match(zipEntries["archive/data.warc"].toString("utf8"), /WARC\/1.1/);
    assert.match(zipEntries["archive/data.warc"].toString("utf8"), /Seed Title|HTTP\/1.1 200/);
    assert.match(zipEntries["pages/pages.jsonl"].toString("utf8"), /"url":"http:\/\/127\.0\.0\.1:/);

    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    assert.equal(cdxEntries.length, 1);
    assert.equal(cdxEntries[0]!.url, `http://127.0.0.1:${originPort}/seed`);
    assert.equal(cdxEntries[0]!.data.status, "200");
    assert.ok(Number.parseInt(cdxEntries[0]!.data.length, 10) > 0);

    const indexedRecord = readIndexedWarcRecord(
      zipEntries["archive/data.warc"],
      cdxEntries[0]!,
    );
    assert.match(indexedRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/seed/);
    assert.match(indexedRecord, /HTTP\/1\.1 200 OK/);
    assert.match(indexedRecord, /Seed Title/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor indexes redirect and final document records from the stored WARC", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(302, {
        location: "/final",
      });
      response.end();
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    assert.equal(request.url, "/final");
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end("<html><head><title>Redirect Final</title></head><body>final</body></html>");
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);

  const proxyServer = createProxyServer();
  proxyServer.listen(0, "127.0.0.1");
  await once(proxyServer, "listening");
  const proxyPort = getListenPort(proxyServer);

  const executor = createExecutor(`http://127.0.0.1:${proxyPort}`);

  try {
    const result = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/seed`,
    }));

    assert.equal(result.exitCode, 0);

    const zipEntries = parseStoredZip(Buffer.from(result.artifacts.wacz ?? []));
    const pages = Buffer.from(result.artifacts.pages ?? []).toString("utf8");
    assert.match(pages, new RegExp(`"url":"http://127\\.0\\.0\\.1:${originPort}/final"`));

    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    assert.deepEqual(
      cdxEntries.map((entry) => [entry.url, entry.data.status]),
      [
        [`http://127.0.0.1:${originPort}/seed`, "302"],
        [`http://127.0.0.1:${originPort}/final`, "200"],
      ],
    );
    assert.equal(cdxEntries[0]!.data.offset, "0");
    assert.ok(Number.parseInt(cdxEntries[0]!.data.length, 10) > 0);
    assert.ok(Number.parseInt(cdxEntries[1]!.data.offset, 10) > 0);
    assert.ok(Number.parseInt(cdxEntries[1]!.data.length, 10) > 0);

    const archive = zipEntries["archive/data.warc"];
    const redirectRecord = readIndexedWarcRecord(archive, cdxEntries[0]!);
    assert.match(redirectRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/seed/);
    assert.match(redirectRecord, /HTTP\/1\.1 302 Found/);
    assert.match(redirectRecord, /location: \/final/);

    const finalRecord = readIndexedWarcRecord(archive, cdxEntries[1]!);
    assert.match(finalRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/final/);
    assert.match(finalRecord, /HTTP\/1\.1 200 OK/);
    assert.match(finalRecord, /Redirect Final/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor isolates sequential runs by restarting the browser when idle", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    if (request.url === "/first") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
        "set-cookie": "seen=first; Path=/",
      });
      response.end(`<!doctype html>
        <html>
          <head><title>First Run</title></head>
          <body>
            <script>
              localStorage.setItem("shared", "first");
              sessionStorage.setItem("shared", "first");
              document.body.textContent = "state written";
            </script>
          </body>
        </html>`);
      return;
    }

    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end(`<!doctype html>
      <html>
        <head><title>Second Run</title></head>
        <body>
          <script>
            document.body.textContent = JSON.stringify({
              cookie: document.cookie,
              localStorage: localStorage.getItem("shared"),
              sessionStorage: sessionStorage.getItem("shared"),
            });
          </script>
        </body>
      </html>`);
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);

  const proxyServer = createProxyServer();
  proxyServer.listen(0, "127.0.0.1");
  await once(proxyServer, "listening");
  const proxyPort = getListenPort(proxyServer);

  const executor = createExecutor(`http://127.0.0.1:${proxyPort}`);

  try {
    const firstResult = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/first`,
    }));
    const secondResult = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/second`,
    }));

    assert.equal(firstResult.exitCode, 0);
    assert.equal(secondResult.exitCode, 0);
    assert.match(Buffer.from(firstResult.artifacts.stdoutLog).toString("utf8"), /reused_browser=false/);
    assert.match(Buffer.from(secondResult.artifacts.stdoutLog).toString("utf8"), /reused_browser=false/);
    assert.notEqual(getBrowserPid(firstResult), getBrowserPid(secondResult));
    assert.doesNotMatch(
      Buffer.from(secondResult.artifacts.wacz ?? []).toString("utf8"),
      /seen=first|localStorage":"first"|sessionStorage":"first"/,
    );
    assert.deepEqual(executor.getPoolStats(), {
      launched: 2,
      idle: 0,
    });
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor launches a browser per concurrent run", { concurrency: false }, async () => {
  const originServer = http.createServer(async (_request, response) => {
    await new Promise((resolve) => setTimeout(resolve, 400));
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end("<html><head><title>Concurrent Title</title></head><body>Hello</body></html>");
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);

  const proxyServer = createProxyServer();
  proxyServer.listen(0, "127.0.0.1");
  await once(proxyServer, "listening");
  const proxyPort = getListenPort(proxyServer);

  const executor = createExecutor(`http://127.0.0.1:${proxyPort}`);

  try {
    const [firstResult, secondResult] = await Promise.all([
      executor.executeRun(createRunRequest({
        url: `http://127.0.0.1:${originPort}/slow-1`,
      })),
      executor.executeRun(createRunRequest({
        url: `http://127.0.0.1:${originPort}/slow-2`,
      })),
    ]);

    assert.equal(firstResult.exitCode, 0);
    assert.equal(secondResult.exitCode, 0);
    assert.notEqual(getBrowserPid(firstResult), getBrowserPid(secondResult));
    assert.equal(executor.getPoolStats().launched, 2);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor closes the browser after each run", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end(`<html><body>${request.url}</body></html>`);
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);

  const proxyServer = createProxyServer();
  proxyServer.listen(0, "127.0.0.1");
  await once(proxyServer, "listening");
  const proxyPort = getListenPort(proxyServer);

  const executor = createExecutor(`http://127.0.0.1:${proxyPort}`);

  try {
    const firstResult = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/first`,
    }));
    const firstPid = getBrowserPid(firstResult);
    assert.throws(() => process.kill(firstPid, 0), /ESRCH/);

    const secondResult = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/second`,
    }));

    assert.equal(firstResult.exitCode, 0);
    assert.equal(secondResult.exitCode, 0);
    assert.match(Buffer.from(secondResult.artifacts.stdoutLog).toString("utf8"), /reused_browser=false/);
    assert.notEqual(firstPid, getBrowserPid(secondResult));
    assert.equal(executor.getPoolStats().launched, 2);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor fails hard when the proxy is unreachable", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    response.writeHead(200, {
      "content-type": "text/html; charset=utf-8",
    });
    response.end("<html><body>origin</body></html>");
  });
  originServer.listen(0, "127.0.0.1");
  await once(originServer, "listening");
  const originPort = getListenPort(originServer);
  const unavailablePort = await reservePort();
  const executor = createExecutor(`http://127.0.0.1:${unavailablePort}`);
  try {
    const result = await executor.executeRun(createRunRequest({
      url: `http://127.0.0.1:${originPort}/seed`,
    }));

    assert.equal(result.exitCode, 9);
    assert.match(
      result.failureDetail ?? "",
      /devtools|ECONNREFUSED|proxy|closed|timed out|ERR_EMPTY_RESPONSE/i,
    );
  } finally {
    await executor.close();
    await closeServer(originServer);
  }
});

function createExecutor(proxyServer: string): UpstreamRunExecutor {
  return new UpstreamRunExecutor({
    browserBin: kBrowserBin,
    geometry: kBrowserGeometry,
    proxyServer,
  });
}

function createRunRequest(options: {
  url: string;
}): RunRequest {
  return {
    url: options.url,
    jobTimeoutMs: 99_000,
  };
}

function createProxyServer(): http.Server {
  return http.createServer((request, response) => {
    const target = new URL(request.url ?? "");
    const upstream = http.request(
      {
        host: target.hostname,
        port: target.port,
        path: `${target.pathname}${target.search}`,
        method: request.method,
        headers: {
          host: target.host,
          "x-webshot-via-proxy": "1",
        },
      },
      (upstreamResponse) => {
        response.writeHead(upstreamResponse.statusCode ?? 502, upstreamResponse.headers);
        upstreamResponse.pipe(response);
      },
    );
    upstream.on("error", (error) => {
      response.writeHead(502, {
        "content-type": "text/plain; charset=utf-8",
      });
      response.end(String(error));
    });
    request.pipe(upstream);
  });
}

async function reservePort(): Promise<number> {
  const server = http.createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const port = getListenPort(server);
  await closeServer(server);
  return port;
}

function getListenPort(server: http.Server): number {
  const address = server.address();
  if (address === null || typeof address === "string") {
    throw new Error("server did not bind to a TCP port");
  }
  return address.port;
}

async function closeServer(server: http.Server): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.close((error) => {
      if (error) {
        reject(error);
        return;
      }
      resolve();
    });
  });
}

function parseStoredZip(bytes: Buffer): Record<string, Buffer> {
  const entries: Record<string, Buffer> = {};
  let offset = 0;

  while (offset + 4 <= bytes.length) {
    const signature = bytes.readUInt32LE(offset);
    if (signature === 0x02014b50 || signature === 0x06054b50) {
      break;
    }
    assert.equal(signature, 0x04034b50);

    const compressedSize = bytes.readUInt32LE(offset + 18);
    const fileNameLength = bytes.readUInt16LE(offset + 26);
    const extraLength = bytes.readUInt16LE(offset + 28);
    const nameStart = offset + 30;
    const bodyStart = nameStart + fileNameLength + extraLength;
    const name = bytes.subarray(nameStart, nameStart + fileNameLength).toString("utf8");
    entries[name] = bytes.subarray(bodyStart, bodyStart + compressedSize);
    offset = bodyStart + compressedSize;
  }

  return entries;
}

function getBrowserPid(result: { artifacts: { stdoutLog: Uint8Array } }): number {
  const stdout = Buffer.from(result.artifacts.stdoutLog).toString("utf8");
  const match = stdout.match(/browser_pid=(\d+)/);
  assert.ok(match);
  return Number.parseInt(match[1]!, 10);
}

function parseCdxj(text: string): Array<{
  url: string;
  timestamp: string;
  data: {
    url: string;
    status: string;
    mime: string;
    filename: string;
    length: string;
    offset: string;
  };
}> {
  return text.trim().split("\n").filter(Boolean).map((line) => {
    const firstSpace = line.indexOf(" ");
    const secondSpace = line.indexOf(" ", firstSpace + 1);
    assert.ok(firstSpace > 0);
    assert.ok(secondSpace > firstSpace);
    const url = line.slice(0, firstSpace);
    const timestamp = line.slice(firstSpace + 1, secondSpace);
    const payload = line.slice(secondSpace + 1);
    assert.ok(url);
    assert.ok(timestamp);
    assert.ok(payload);
    return {
      url,
      timestamp,
      data: JSON.parse(payload) as {
        url: string;
        status: string;
        mime: string;
        filename: string;
        length: string;
        offset: string;
      },
    };
  });
}

function readIndexedWarcRecord(
  archive: Buffer,
  entry: {
    data: {
      offset: string;
      length: string;
    };
  },
): string {
  const offset = Number.parseInt(entry.data.offset, 10);
  const length = Number.parseInt(entry.data.length, 10);
  assert.ok(Number.isInteger(offset));
  assert.ok(Number.isInteger(length));
  assert.ok(length > 0);
  return archive.subarray(offset, offset + length).toString("utf8");
}
