import type { RunRequest } from "../src/contracts.js";
import type { RunExecutionResult } from "../src/run_executor.js";
import { captureViaProxy } from "./http_capture.js";
import type { BrowserPool } from "./browser_pool.js";
import { buildSuccessStdoutLog, buildFailureStderrLog } from "./logs.js";
import { buildPagesJsonl } from "./pages.js";
import { buildWacz } from "./wacz.js";
import { buildWarc } from "./warc.js";

export type BrowsertrixCaptureOptions = {
  browserBin: string;
  geometry?: string;
  browserPool: BrowserPool;
  proxyServer?: string;
};

const kPageLoadTimeoutMs = 10_000;
const kPostLoadDelayMs = 1_000;
const kNetIdleWaitMs = 0;
const kPageExtraDelayMs = 0;
const kBehaviorTimeoutMs = 1_000;
const kMaxBodyBytes = 50 * 1024 * 1024;
const kLang = "en";
const kScopeType = "page-spa" as const;

export async function runBrowsertrixCapture(
  run: RunRequest,
  options: BrowsertrixCaptureOptions,
): Promise<RunExecutionResult> {
  try {
    const capture = await captureViaProxy(run.url, {
      browserPool: options.browserPool,
      pageLoadTimeoutMs: kPageLoadTimeoutMs,
      postLoadDelayMs: kPostLoadDelayMs,
      netIdleWaitMs: kNetIdleWaitMs,
      pageExtraDelayMs: kPageExtraDelayMs,
      behaviorTimeoutMs: kBehaviorTimeoutMs,
      jobTimeoutMs: run.jobTimeoutMs,
      maxBodyBytes: kMaxBodyBytes,
      lang: kLang,
      scopeType: kScopeType,
    });
    const exchange = capture.exchange;

    const pages = buildPagesJsonl(exchange);
    const stdoutLog = buildSuccessStdoutLog(run, exchange, {
      browserBin: options.browserBin,
      geometry: options.geometry,
      browserPid: capture.metadata.browserPid,
      reusedBrowser: capture.metadata.reusedBrowser,
    });
    const stderrLog = new Uint8Array();
    const warc = buildWarc(exchange);
    const wacz = buildWacz(run, pages, warc, stdoutLog, stderrLog);
    const bounded = applyArtifactMemoryLimit({
      wacz,
      pages,
      stdoutLog,
      stderrLog,
    });

    return {
      exitCode: bounded.exitCode ?? (exchange.statusCode >= 400 ? 9 : 0),
      seedProbe: {
        status: exchange.statusCode,
        load_state: bounded.exitCode !== undefined || exchange.statusCode >= 400 ? 0 : 2,
      },
      ...(
        bounded.failureDetail !== undefined
          ? { failureDetail: bounded.failureDetail }
          : exchange.statusCode >= 400
            ? { failureDetail: `seed returned HTTP ${exchange.statusCode}` }
            : {}
      ),
      artifacts: bounded.artifacts,
    };
  } catch (error) {
    return {
      exitCode: 9,
      failureDetail: error instanceof Error ? error.message : String(error),
      artifacts: {
        stdoutLog: new Uint8Array(),
        stderrLog: buildFailureStderrLog(error),
      },
    };
  }
}

function applyArtifactMemoryLimit(artifacts: {
  wacz: Uint8Array;
  pages: Uint8Array;
  stdoutLog: Uint8Array;
  stderrLog: Uint8Array;
}): {
  exitCode?: number;
  failureDetail?: string;
  artifacts: {
    wacz?: Uint8Array;
    pages?: Uint8Array;
    stdoutLog: Uint8Array;
    stderrLog: Uint8Array;
  };
} {
  const retainedBytes = [
    artifacts.wacz,
    artifacts.pages,
    artifacts.stdoutLog,
    artifacts.stderrLog,
  ].reduce((sum, value) => sum + value.byteLength, 0);
  if (retainedBytes <= kMaxBodyBytes) {
    return { artifacts };
  }

  const failureDetail = `retained artifact bytes ${retainedBytes} exceeded size limit ${kMaxBodyBytes}`;
  return {
    exitCode: 14,
    failureDetail,
    artifacts: {
      stdoutLog: artifacts.stdoutLog,
      stderrLog: Buffer.concat([artifacts.stderrLog, Buffer.from(`${failureDetail}\n`, "utf8")]),
    },
  };
}
