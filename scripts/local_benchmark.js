#!/usr/bin/env node

import http from 'node:http';

const MOCK_PORT = 9099;
const PROXY_PORT = 17777;
const TOTAL_REQUESTS = 5000;
const CONCURRENCY = 50;

// 1. Start zero-latency mock upstream server
const mockServer = http.createServer((req, res) => {
  res.writeHead(200, {
    'Content-Type': 'application/json',
    'Content-Length': '27',
    'Connection': 'close',
  });
  res.end('{"status":"ok","speed":"c"}');
});

mockServer.listen(MOCK_PORT, '127.0.0.1', async () => {
  console.log(`\n==========================================================`);
  console.log(`       LOCAL ZERO-LATENCY PROXY BENCHMARK`);
  console.log(`==========================================================`);
  console.log(`Target Proxy   : http://127.0.0.1:${PROXY_PORT}`);
  console.log(`Mock Upstream  : http://127.0.0.1:${MOCK_PORT}`);
  console.log(`Total Requests : ${TOTAL_REQUESTS}`);
  console.log(`Concurrency    : ${CONCURRENCY} parallel clients\n`);

  const latencies = [];
  const start = performance.now();
  let completed = 0;
  let success = 0;
  let failed = 0;
  let inFlight = 0;
  let nextIdx = 0;

  await new Promise((resolve) => {
    function launch() {
      while (inFlight < CONCURRENCY && nextIdx < TOTAL_REQUESTS) {
        nextIdx++;
        inFlight++;
        const reqStart = performance.now();

        const req = http.request(
          `http://127.0.0.1:${PROXY_PORT}/?url=http://127.0.0.1:${MOCK_PORT}/test`,
          { headers: { Connection: 'close' } },
          (res) => {
            res.resume();
            res.on('end', () => {
              const reqDuration = performance.now() - reqStart;
              latencies.push(reqDuration);
              if (res.statusCode === 200) {
                success++;
              } else {
                failed++;
              }
              completed++;
              inFlight--;
              if (completed === TOTAL_REQUESTS) {
                resolve();
              } else {
                launch();
              }
            });
          }
        );

        req.on('error', () => {
          failed++;
          completed++;
          inFlight--;
          if (completed === TOTAL_REQUESTS) resolve();
          else launch();
        });

        req.end();
      }
    }
    launch();
  });

  const totalDurationSec = (performance.now() - start) / 1000;
  latencies.sort((a, b) => a - b);
  const minLat = latencies.length ? latencies[0].toFixed(2) : '0';
  const avgLat = latencies.length ? (latencies.reduce((a, b) => a + b, 0) / latencies.length).toFixed(2) : '0';
  const p95Lat = latencies.length ? latencies[Math.floor(latencies.length * 0.95)].toFixed(2) : '0';
  const maxLat = latencies.length ? latencies[latencies.length - 1].toFixed(2) : '0';
  const rps = (success / totalDurationSec).toFixed(1);

  console.log(`--- ⏱️ THROUGHPUT & PERFORMANCE ---`);
  console.log(`Completed Requests : ${completed} / ${TOTAL_REQUESTS} (${((success/TOTAL_REQUESTS)*100).toFixed(1)}% success)`);
  console.log(`Total Time         : ${totalDurationSec.toFixed(2)} seconds`);
  console.log(`Throughput         : ${rps} requests / sec`);
  console.log(`Latency (Min)      : ${minLat} ms`);
  console.log(`Latency (Avg)      : ${avgLat} ms`);
  console.log(`Latency (p95)      : ${p95Lat} ms`);
  console.log(`Latency (Max)      : ${maxLat} ms`);
  console.log(`==========================================================\n`);

  mockServer.close();
  process.exit(failed > 0 ? 1 : 0);
});
