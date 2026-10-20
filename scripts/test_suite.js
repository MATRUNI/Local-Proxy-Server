#!/usr/bin/env node

import fs from 'node:fs';

const PROXY_URL = 'http://127.0.0.1:17777';
const REPORT_FILE = 'logs/test_suite_report.log';

// ANSI terminal colors
const C = {
  reset: '\x1b[0m',
  bold: '\x1b[1m',
  green: '\x1b[32m',
  red: '\x1b[31m',
  yellow: '\x1b[33m',
  cyan: '\x1b[36m',
  blue: '\x1b[34m',
  dim: '\x1b[2m',
};

// 1. The Real-World Web Pages & Protocol Matrix
const testCases = [
  {
    name: 'Google Homepage (Full HTML)',
    url: 'https://www.google.com/',
    method: 'GET',
    expectedStatus: 200,
    headers: { 'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64)' },
    validate: (res, body) => body.toLowerCase().includes('google') || body.includes('<title>'),
  },
  {
    name: 'Hacker News Frontpage (HTML/Cloudflare)',
    url: 'https://news.ycombinator.com/',
    method: 'GET',
    expectedStatus: 200,
    headers: { 'User-Agent': 'Mozilla/5.0' },
    validate: (res, body) => body.includes('Hacker News'),
  },
  {
    name: 'Wikipedia - C Language Article (~150KB)',
    url: 'https://en.wikipedia.org/wiki/C_(programming_language)',
    method: 'GET',
    expectedStatus: 200,
    headers: { 'User-Agent': 'C-Proxy-Benchmark/1.0 (https://github.com/)' },
    validate: (res, body) => body.includes('Dennis Ritchie') || body.includes('programming'),
  },
  {
    name: 'DuckDuckGo Search Engine (HTML)',
    url: 'https://html.duckduckgo.com/html/',
    method: 'GET',
    expectedStatus: 200,
    headers: { 'User-Agent': 'Mozilla/5.0' },
    validate: (res, body) => body.includes('DuckDuckGo'),
  },
  {
    name: 'Plain HTTP Port 80 - Example.com',
    url: 'http://example.com/',
    method: 'GET',
    expectedStatus: 200,
    validate: (res, body) => body.includes('Example Domain'),
  },
  {
    name: 'Cloudflare CDN - REST JSON API',
    url: 'https://jsonplaceholder.typicode.com/posts/1',
    method: 'GET',
    expectedStatus: 200,
    validate: (res, body) => body.includes('"userId": 1'),
  },
  {
    name: 'POST Body Streaming - Upstream JSON',
    url: 'https://httpbin.org/post',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ project: 'C-Proxy-C10K', author: 'Himanshu', level: 9000 }),
    expectedStatus: 200,
    validate: (res, body) => body.includes('C-Proxy-C10K'),
  },
  {
    name: 'CORS Preflight - OPTIONS Check',
    url: 'https://httpbin.org/get',
    method: 'OPTIONS',
    expectedStatus: 204,
    validate: (res) => res.headers.has('access-control-allow-origin'),
  },
  {
    name: 'HTTP 404 - Not Found Passthrough',
    url: 'https://httpbin.org/status/404',
    method: 'GET',
    expectedStatus: 404,
  },
  {
    name: 'DNS Failure - 502 Bad Gateway',
    url: 'https://thisdomainwillneverexist999888777.xyz',
    method: 'GET',
    expectedStatus: 502,
  },
  {
    name: 'Missing URL - 400 Bad Request',
    rawUrl: `${PROXY_URL}/`, // No ?url= param
    method: 'GET',
    expectedStatus: 400,
  },
];

async function runSingleTest(tc) {
  const target = tc.rawUrl || `${PROXY_URL}/?url=${encodeURIComponent(tc.url)}`;
  const start = performance.now();
  let status = 0;
  let errorMsg = null;
  let body = '';
  let passed = false;

  try {
    const res = await fetch(target, {
      method: tc.method || 'GET',
      headers: tc.headers || {},
      body: tc.body || undefined,
    });
    status = res.status;
    body = await res.text();

    const statusMatch = status === tc.expectedStatus;
    const bodyValid = tc.validate ? tc.validate(res, body) : true;
    passed = statusMatch && bodyValid;

    if (!statusMatch) errorMsg = `Expected HTTP ${tc.expectedStatus}, got ${status}`;
    else if (!bodyValid) errorMsg = `Content validation failed on returned payload`;
  } catch (err) {
    errorMsg = err.message;
  }

  const durationMs = Math.round(performance.now() - start);
  const sizeBytes = Buffer.byteLength(body, 'utf8');
  return { ...tc, status, durationMs, sizeBytes, passed, errorMsg, bodyPreview: body.slice(0, 80) };
}

async function main() {
  console.log(`\n${C.bold}${C.cyan}╔════════════════════════════════════════════════════════════════╗${C.reset}`);
  console.log(`${C.bold}${C.cyan}║   C REVERSE PROXY — REAL WEBPAGES & HIGH-LOAD TEST SUITE      ║${C.reset}`);
  console.log(`${C.bold}${C.cyan}╚════════════════════════════════════════════════════════════════╝${C.reset}\n`);

  fs.mkdirSync('logs', { recursive: true });
  const logStream = fs.createWriteStream(REPORT_FILE, { flags: 'w' });
  const log = (msg) => {
    console.log(msg);
    logStream.write(msg.replace(/\x1b\[[0-9;]*m/g, '') + '\n');
  };

  log(`Timestamp   : ${new Date().toISOString()}`);
  log(`Target Proxy: ${PROXY_URL}\n`);

  // Phase 1: Real-World Webpages & Heavyweight HTML
  log(`${C.bold}[PHASE 1] Loading Real Webpages, HTML Payloads & APIs simultaneously...${C.reset}`);
  const phase1Results = await Promise.all(testCases.map(runSingleTest));

  phase1Results.forEach((r) => {
    const tag = r.passed ? `${C.green}✔ PASS${C.reset}` : `${C.red}✖ FAIL${C.reset}`;
    const statusTag = r.status ? `HTTP ${r.status}` : 'ERR';
    const time = `${r.durationMs}ms`.padStart(7);
    const size = r.sizeBytes > 1024 ? `${(r.sizeBytes / 1024).toFixed(1)} KB`.padStart(9) : `${r.sizeBytes} B`.padStart(9);
    log(`  [${tag}] ${C.bold}${r.name.padEnd(42)}${C.reset} [${statusTag}] ${C.blue}${size}${C.reset} in ${C.yellow}${time}${C.reset}`);
    if (!r.passed) {
      log(`         ${C.red}↳ Error: ${r.errorMsg}${C.reset}`);
    }
  });

  // Phase 2: Parallel Concurrency Burst across Real Webpages (50 requests simultaneously)
  const BURST_COUNT = 50;
  log(`\n${C.bold}[PHASE 2] Firing Parallel Burst (${BURST_COUNT} simultaneous requests across Google, Hacker News, Wikipedia)...${C.reset}`);

  const burstTargets = [
    { url: 'https://www.google.com/', name: 'Google' },
    { url: 'https://news.ycombinator.com/', name: 'HackerNews' },
    { url: 'http://example.com/', name: 'Example.com' },
    { url: 'https://jsonplaceholder.typicode.com/posts/1', name: 'JSONPlaceholder' },
  ];

  const burstStart = performance.now();
  const burstPromises = Array.from({ length: BURST_COUNT }, (_, idx) => {
    const t = burstTargets[idx % burstTargets.length];
    return runSingleTest({
      name: `Burst #${idx + 1} (${t.name})`,
      url: t.url,
      method: 'GET',
      expectedStatus: 200,
      headers: { 'User-Agent': 'Mozilla/5.0 (C-Proxy-Burst)' },
    });
  });

  const burstResults = await Promise.all(burstPromises);
  const totalBurstTime = performance.now() - burstStart;

  const burstPassed = burstResults.filter((r) => r.passed).length;
  const burstFailed = BURST_COUNT - burstPassed;
  const latencies = burstResults.map((r) => r.durationMs).sort((a, b) => a - b);
  const minLat = latencies[0];
  const maxLat = latencies[latencies.length - 1];
  const avgLat = (latencies.reduce((a, b) => a + b, 0) / latencies.length).toFixed(1);
  const rps = ((BURST_COUNT / totalBurstTime) * 1000).toFixed(1);
  const totalBytesStreamed = burstResults.reduce((acc, r) => acc + (r.sizeBytes || 0), 0);

  log(`  Completed in : ${C.yellow}${(totalBurstTime / 1000).toFixed(2)}s${C.reset}`);
  log(`  Throughput   : ${C.bold}${rps} req/sec${C.reset}`);
  log(`  Data Streamed: ${C.bold}${(totalBytesStreamed / 1024).toFixed(1)} KB${C.reset}`);
  log(`  Pass / Fail  : ${C.green}${burstPassed} passed${C.reset}, ${burstFailed > 0 ? C.red : C.dim}${burstFailed} failed${C.reset}`);
  log(`  Latency      : Min ${minLat}ms | Avg ${avgLat}ms | Max ${maxLat}ms`);

  // Final Summary
  const allResults = [...phase1Results, ...burstResults];
  const totalPass = allResults.filter((r) => r.passed).length;
  const totalFail = allResults.length - totalPass;

  log(`\n${C.bold}================================================================${C.reset}`);
  log(`                       FINAL REPORT                             `);
  log(`================================================================`);
  log(`  Total Requests Executed : ${allResults.length}`);
  log(`  Success Rate            : ${totalPass === allResults.length ? C.green : C.yellow}${((totalPass / allResults.length) * 100).toFixed(1)}%${C.reset}`);
  log(`  Report Saved To         : ${REPORT_FILE}`);
  log(`================================================================\n`);

  logStream.end(() => {
    process.exit(totalFail === 0 ? 0 : 1);
  });
}

main();
