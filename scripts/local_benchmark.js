#!/usr/bin/env node

/**
 * scripts/local_benchmark.js
 * 
 * High-performance zero-latency local benchmark suite.
 * Supports persistent HTTP Keep-Alive, multi-process clustering,
 * and standard single-process connection benchmarking.
 */

import http from 'node:http';
import cluster from 'node:cluster';
import os from 'node:os';

const MOCK_PORT = 9099;
const PROXY_PORT = 17777;
const TOTAL_REQUESTS = 20000;
const CONCURRENCY = 100;

const args = process.argv.slice(2);
const useKeepAlive = !args.includes('--no-keepalive');
const useCluster = !args.includes('--single');
const numWorkers = Math.min(4, os.cpus().length);

if (useCluster && cluster.isPrimary) {
  // 1. Primary process: Start zero-latency mock upstream server
  const mockServer = http.createServer((req, res) => {
    res.writeHead(200, {
      'Content-Type': 'application/json',
      'Content-Length': '27',
      'Connection': useKeepAlive ? 'keep-alive' : 'close',
    });
    res.end('{"status":"ok","speed":"c"}');
  });

  mockServer.listen(MOCK_PORT, '127.0.0.1', () => {
    console.log(`\n===============================================================`);
    console.log(`        LOCAL HIGH-THROUGHPUT PROXY BENCHMARK                  `);
    console.log(`===============================================================`);
    console.log(`Target Proxy   : http://127.0.0.1:${PROXY_PORT}`);
    console.log(`Mock Upstream  : http://127.0.0.1:${MOCK_PORT}`);
    console.log(`Total Requests : ${TOTAL_REQUESTS}`);
    console.log(`Concurrency    : ${CONCURRENCY} connections (${useKeepAlive ? 'Keep-Alive' : 'Short-Lived'})`);
    console.log(`Mode           : Multi-Process Cluster (${numWorkers} client workers)\n`);

    const reqsPerWorker = Math.floor(TOTAL_REQUESTS / numWorkers);
    const connsPerWorker = Math.floor(CONCURRENCY / numWorkers);

    let completedWorkers = 0;
    let totalSuccess = 0;
    let totalFailed = 0;
    const startTime = performance.now();

    for (let i = 0; i < numWorkers; i++) {
      const worker = cluster.fork({
        WORKER_REQS: reqsPerWorker,
        WORKER_CONNS: connsPerWorker,
        USE_KEEPALIVE: useKeepAlive ? '1' : '0',
      });

      worker.on('message', (msg) => {
        totalSuccess += msg.success;
        totalFailed += msg.failed;
        completedWorkers++;

        if (completedWorkers === numWorkers) {
          const totalDurationSec = (performance.now() - startTime) / 1000;
          const rps = ((totalSuccess + totalFailed) / totalDurationSec).toFixed(1);

          console.log(`---------------- RESULTS ----------------`);
          console.log(`Successful Requests : ${totalSuccess} / ${TOTAL_REQUESTS} (${((totalSuccess / TOTAL_REQUESTS) * 100).toFixed(1)}%)`);
          console.log(`Failed Requests     : ${totalFailed}`);
          console.log(`Total Time          : ${totalDurationSec.toFixed(2)} seconds`);
          console.log(`🔥 THROUGHPUT       : ${rps} req/sec`);
          console.log(`===============================================================\n`);

          mockServer.close();
          process.exit(totalFailed > 0 ? 1 : 0);
        }
      });
    }
  });
} else {
  // Worker process (or single-process mode)
  const reqsToRun = parseInt(process.env.WORKER_REQS || TOTAL_REQUESTS, 10);
  const concurrencyToRun = parseInt(process.env.WORKER_CONNS || CONCURRENCY, 10);
  const keepAlive = process.env.USE_KEEPALIVE !== '0';

  const agent = new http.Agent({
    keepAlive,
    maxSockets: concurrencyToRun,
    keepAliveMsecs: 10000,
  });

  const targetPath = `/?url=${encodeURIComponent(`http://127.0.0.1:${MOCK_PORT}/test`)}`;
  let completed = 0;
  let success = 0;
  let failed = 0;
  let inFlight = 0;
  let launched = 0;

  function launch() {
    while (inFlight < concurrencyToRun && launched < reqsToRun) {
      launched++;
      inFlight++;

      const req = http.request(
        {
          hostname: '127.0.0.1',
          port: PROXY_PORT,
          path: targetPath,
          method: 'GET',
          agent,
          headers: {
            Connection: keepAlive ? 'keep-alive' : 'close',
          },
        },
        (res) => {
          res.resume();
          res.on('end', () => {
            if (res.statusCode === 200) success++;
            else failed++;
            completed++;
            inFlight--;

            if (completed === reqsToRun) {
              if (process.send) {
                process.send({ success, failed });
              }
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
        if (completed === reqsToRun) {
          if (process.send) {
            process.send({ success, failed });
          }
        } else {
          launch();
        }
      });

      req.end();
    }
  }

  launch();
}
