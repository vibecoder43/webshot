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
    const pagesLines = Buffer.from(result.artifacts.pages ?? []).toString("utf8").trim().split("\n");
    assert.equal(pagesLines.length, 1);
    const seedPage = JSON.parse(pagesLines[0]!) as {
      id: string;
      url: string;
      title: string;
      loadState: number;
      mime: string;
      seed: boolean;
      ts: string;
      status: number;
      depth: number;
    };
    assert.match(seedPage.id, /^[0-9a-f-]{36}$/);
    assert.equal(seedPage.url, `http://127.0.0.1:${originPort}/seed`);
    assert.equal(seedPage.title, "Seed Title");
    assert.equal(seedPage.loadState, 2);
    assert.equal(seedPage.mime, "text/html; charset=utf-8");
    assert.equal(seedPage.seed, true);
    assert.match(seedPage.ts, /^\d{4}-\d{2}-\d{2}T/);
    assert.equal(seedPage.status, 200);
    assert.equal(seedPage.depth, 0);

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
    assert.match(zipEntries["pages/pages.jsonl"].toString("utf8"), /"format":"json-pages-1.0"/);
    const datapackage = JSON.parse(zipEntries["datapackage.json"].toString("utf8")) as {
      resources: Array<{
        path: string;
      }>;
    };
    assert.ok(datapackage.resources.some((resource) => resource.path === "archive/data.warc"));

    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    const seedEntry = cdxEntries.find((entry) => entry.url === `http://127.0.0.1:${originPort}/seed`);
    assert.ok(seedEntry);
    assert.equal(seedEntry.key, `http://127.0.0.1:${originPort}/seed`);
    assert.equal(seedEntry.data.status, "200");
    assert.equal(seedEntry.data.filename, "data.warc");
    assert.ok(Number.parseInt(seedEntry.data.length, 10) > 0);

    const indexedRecord = readIndexedWarcRecord(
      zipEntries["archive/data.warc"],
      seedEntry,
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
    const redirectEntry = cdxEntries.find((entry) => entry.url === `http://127.0.0.1:${originPort}/seed`);
    const finalEntry = cdxEntries.find((entry) => entry.url === `http://127.0.0.1:${originPort}/final`);
    assert.ok(redirectEntry);
    assert.ok(finalEntry);
    assert.equal(redirectEntry.data.status, "302");
    assert.equal(finalEntry.data.status, "200");
    assert.equal(redirectEntry.data.offset, "0");
    assert.ok(Number.parseInt(redirectEntry.data.length, 10) > 0);
    assert.ok(Number.parseInt(finalEntry.data.offset, 10) > 0);
    assert.ok(Number.parseInt(finalEntry.data.length, 10) > 0);

    const archive = zipEntries["archive/data.warc"];
    const redirectRecord = readIndexedWarcRecord(archive, redirectEntry);
    assert.match(redirectRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/seed/);
    assert.match(redirectRecord, /HTTP\/1\.1 302 Found/);
    assert.match(redirectRecord, /location: \/final/);

    const finalRecord = readIndexedWarcRecord(archive, finalEntry);
    assert.match(finalRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/final/);
    assert.match(finalRecord, /HTTP\/1\.1 200 OK/);
    assert.match(finalRecord, /Redirect Final/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor stores subresources as indexed WARC records", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
      });
      response.end(`<!doctype html>
        <html>
          <head>
            <title>Seed With Assets</title>
            <link rel="stylesheet" href="/style.css">
            <script defer src="/script.js"></script>
          </head>
          <body>seed</body>
        </html>`);
      return;
    }
    if (request.url === "/style.css") {
      response.writeHead(200, {
        "content-type": "text/css; charset=utf-8",
      });
      response.end("body { color: rgb(1, 2, 3); }");
      return;
    }
    if (request.url === "/script.js") {
      response.writeHead(200, {
        "content-type": "text/javascript; charset=utf-8",
      });
      response.end("window.__assetLoaded = true;");
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    response.writeHead(404);
    response.end();
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
    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    const archive = zipEntries["archive/data.warc"];
    const urls = new Set(cdxEntries.map((entry) => entry.url));
    assert.ok(urls.has(`http://127.0.0.1:${originPort}/seed`));
    assert.ok(urls.has(`http://127.0.0.1:${originPort}/style.css`));
    assert.ok(urls.has(`http://127.0.0.1:${originPort}/script.js`));

    const styleEntry = cdxEntries.find((entry) => entry.url.endsWith("/style.css"));
    const scriptEntry = cdxEntries.find((entry) => entry.url.endsWith("/script.js"));
    assert.ok(styleEntry);
    assert.ok(scriptEntry);

    const styleRecord = readIndexedWarcRecord(archive, styleEntry);
    assert.match(styleRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/style\.css/);
    assert.match(styleRecord, /HTTP\/1\.1 200 OK/);
    assert.match(styleRecord, /body \{ color: rgb\(1, 2, 3\); \}/);

    const scriptRecord = readIndexedWarcRecord(archive, scriptEntry);
    assert.match(scriptRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/script\.js/);
    assert.match(scriptRecord, /HTTP\/1\.1 200 OK/);
    assert.match(scriptRecord, /window\.__assetLoaded = true;/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor preserves non-GET subresource methods in WARC requests", { concurrency: false }, async () => {
  let submitMethod = "";
  let submitBody = "";

  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
      });
      response.end(`<!doctype html>
        <html>
          <head><title>Seed With Post Fetch</title></head>
          <body>
            <script>
              fetch("/submit?source=page", {
                method: "POST",
                headers: {
                  "content-type": "text/plain; charset=utf-8",
                },
                body: "hello=world",
              });
            </script>
          </body>
        </html>`);
      return;
    }
    if (request.url === "/submit?source=page") {
      submitMethod = request.method ?? "";
      request.setEncoding("utf8");
      request.on("data", (chunk) => {
        submitBody += chunk;
      });
      request.on("end", () => {
        response.writeHead(200, {
          "content-type": "text/plain; charset=utf-8",
        });
        response.end("submitted");
      });
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    response.writeHead(404);
    response.end();
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
    assert.equal(submitMethod, "POST");
    assert.equal(submitBody, "hello=world");

    const zipEntries = parseStoredZip(Buffer.from(result.artifacts.wacz ?? []));
    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    const submitEntry = cdxEntries.find((entry) =>
      entry.url === `http://127.0.0.1:${originPort}/submit?source=page`
    );
    assert.ok(submitEntry);
    assert.equal(submitEntry.data.status, "200");

    const archiveText = zipEntries["archive/data.warc"].toString("utf8");
    assert.match(archiveText, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/submit\?source=page/);
    assert.match(archiveText, /POST \/submit\?source=page HTTP\/1\.1/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor keeps successful HEAD subresources in WARC output", { concurrency: false }, async () => {
  let metadataMethod = "";

  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
      });
      response.end(`<!doctype html>
        <html>
          <head><title>Seed With Head Fetch</title></head>
          <body>
            <script type="module">
              await fetch("/metadata?source=page", {
                method: "HEAD",
              });
            </script>
          </body>
        </html>`);
      return;
    }
    if (request.url === "/metadata?source=page") {
      metadataMethod = request.method ?? "";
      response.writeHead(200, {
        "content-type": "text/plain; charset=utf-8",
        "content-length": "9",
      });
      response.end("metadata!");
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    response.writeHead(404);
    response.end();
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
    assert.equal(metadataMethod, "HEAD");

    const zipEntries = parseStoredZip(Buffer.from(result.artifacts.wacz ?? []));
    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    const metadataEntry = cdxEntries.find((entry) =>
      entry.url === `http://127.0.0.1:${originPort}/metadata?source=page`
    );
    assert.ok(metadataEntry);
    assert.equal(metadataEntry.data.status, "200");

    const metadataRecord = readIndexedWarcRecord(zipEntries["archive/data.warc"], metadataEntry);
    assert.match(metadataRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/metadata\?source=page/);
    assert.match(metadataRecord, /HTTP\/1\.1 200 OK/);

    const archiveText = zipEntries["archive/data.warc"].toString("utf8");
    assert.match(archiveText, /HEAD \/metadata\?source=page HTTP\/1\.1/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor preserves redirected subresource hops in WARC output", { concurrency: false }, async () => {
  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
      });
      response.end(`<!doctype html>
        <html>
          <head>
            <title>Seed With Redirected Asset</title>
            <script defer src="/script.js"></script>
          </head>
          <body>seed</body>
        </html>`);
      return;
    }
    if (request.url === "/script.js") {
      response.writeHead(302, {
        location: "/script-final.js",
      });
      response.end();
      return;
    }
    if (request.url === "/script-final.js") {
      response.writeHead(200, {
        "content-type": "text/javascript; charset=utf-8",
      });
      response.end("window.__redirectedAssetLoaded = true;");
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    response.writeHead(404);
    response.end();
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
    const cdxEntries = parseCdxj(zipEntries["indexes/index.cdxj"].toString("utf8"));
    const redirectEntry = cdxEntries.find((entry) => entry.url === `http://127.0.0.1:${originPort}/script.js`);
    const finalEntry = cdxEntries.find((entry) => entry.url === `http://127.0.0.1:${originPort}/script-final.js`);
    assert.ok(redirectEntry);
    assert.ok(finalEntry);
    assert.equal(redirectEntry.data.status, "302");
    assert.equal(finalEntry.data.status, "200");
    assert.ok(Number.parseInt(redirectEntry.data.offset, 10) < Number.parseInt(finalEntry.data.offset, 10));

    const archive = zipEntries["archive/data.warc"];
    const redirectRecord = readIndexedWarcRecord(archive, redirectEntry);
    assert.match(redirectRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/script\.js/);
    assert.match(redirectRecord, /HTTP\/1\.1 302 Found/);
    assert.match(redirectRecord, /location: \/script-final\.js/);

    const finalRecord = readIndexedWarcRecord(archive, finalEntry);
    assert.match(finalRecord, /WARC-Target-URI: http:\/\/127\.0\.0\.1:\d+\/script-final\.js/);
    assert.match(finalRecord, /HTTP\/1\.1 200 OK/);
    assert.match(finalRecord, /window\.__redirectedAssetLoaded = true;/);
  } finally {
    await executor.close();
    await closeServer(proxyServer);
    await closeServer(originServer);
  }
});

test("UpstreamRunExecutor fails before building artifacts when retained subresource bodies exceed the size limit", { concurrency: false }, async () => {
  const largeAsset = `/*${"x".repeat(26 * 1024 * 1024)}*/`;
  const largeAssetBytes = Buffer.byteLength(largeAsset, "utf8");

  const originServer = http.createServer((request, response) => {
    if (request.url === "/seed") {
      response.writeHead(200, {
        "content-type": "text/html; charset=utf-8",
      });
      response.end(`<!doctype html>
        <html>
          <head>
            <title>Seed With Large Assets</title>
            <script defer src="/large-1.js"></script>
            <script defer src="/large-2.js"></script>
          </head>
          <body>seed</body>
        </html>`);
      return;
    }
    if (request.url === "/large-1.js" || request.url === "/large-2.js") {
      response.writeHead(200, {
        "content-type": "text/javascript; charset=utf-8",
        "content-length": `${largeAssetBytes}`,
      });
      response.end(largeAsset);
      return;
    }
    if (request.url === "/favicon.ico") {
      response.writeHead(204);
      response.end();
      return;
    }
    response.writeHead(404);
    response.end();
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

    assert.equal(result.exitCode, 9);
    assert.match(result.failureDetail ?? "", /retained body bytes \d+ exceeded size limit 52428800/);
    assert.equal(result.artifacts.wacz, undefined);
    assert.equal(result.artifacts.pages, undefined);
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
      /devtools|ECONNREFUSED|proxy|closed|timed out|ERR_EMPTY_RESPONSE|net::ERR_[A-Z_]+/i,
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
  key: string;
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
    const key = line.slice(0, firstSpace);
    const timestamp = line.slice(firstSpace + 1, secondSpace);
    const payload = line.slice(secondSpace + 1);
    assert.ok(key);
    assert.ok(timestamp);
    assert.ok(payload);
    const data = JSON.parse(payload) as {
      url: string;
      status: string;
      mime: string;
      filename: string;
      length: string;
      offset: string;
    };
    return {
      key,
      url: data.url,
      timestamp,
      data,
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
