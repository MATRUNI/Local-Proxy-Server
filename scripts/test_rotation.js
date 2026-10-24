#!/usr/bin/env node

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';

const MOCK_PORT = 9099;
const PROXY_PORT = 17777;
const LOGS_DIR = path.resolve('logs');
const BATCH_SIZE = 50; // concurrency

function getLogsStatus() {
  if (!fs.existsSync(LOGS_DIR)) return { files: [], activeFile: null };
  const files = fs.readdirSync(LOGS_DIR)
    .filter(f => f.startsWith('@logger_') || (f.startsWith('logger_') && f.endsWith('.log')))
    .sort();
  const activeFile = files.find(f => f.startsWith('@')) || null;
  const fileDetails = files.map(f => {
    const stat = fs.statSync(path.join(LOGS_DIR, f));
    return { name: f, size: stat.size, active: f.startsWith('@') };
  });
  return { files: fileDetails, activeFile };
}

async function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitForProxy(port, maxTries = 30) {
  for (let i = 0; i < maxTries; i++) {
    try {
      await new Promise((resolve, reject) => {
        const req = http.get(`http://127.0.0.1:${port}/health`, res => {
          res.resume();
          resolve();
        });
        req.on('error', reject);
        req.setTimeout(200, () => {
          req.destroy();
          reject(new Error('timeout'));
        });
      });
      return true;
    } catch {
      await sleep(100);
    }
  }
  return false;
}

// 1. Start mock upstream
const mockServer = http.createServer((req, res) => {
  res.writeHead(200, {
    'Content-Type': 'application/json',
    'Content-Length': '27',
    'Connection': 'close',
  });
  res.end('{"status":"ok","proxy":"c"}');
});

mockServer.listen(MOCK_PORT, '127.0.0.1', async () => {
  console.log('===============================================================');
  console.log('       LIVE ROTATION & COMMIT-BEFORE-RETIRE TEST SUITE        ');
  console.log('===============================================================');

  const initialStatus = getLogsStatus();
  console.log(`Initial Active Log File : ${initialStatus.activeFile || 'None'}`);
  console.log('Files currently in logs/:');
  initialStatus.files.forEach(f => {
    console.log(`  - ${f.name.padEnd(20)} : ${(f.size / 1024).toFixed(1)} KB ${f.active ? '(ACTIVE)' : ''}`);
  });

  // 2. Start Proxy
  console.log('\nStarting Proxy Server (./build/api -p 17777)...');
  const proxy = spawn('./build/api', ['-p', String(PROXY_PORT)], {
    stdio: ['ignore', 'inherit', 'inherit']
  });

  const ready = await waitForProxy(PROXY_PORT);
  if (!ready) {
    console.error('❌ Failed to connect to proxy on port 17777');
    proxy.kill('SIGINT');
    mockServer.close();
    process.exit(1);
  }
  console.log('✔ Proxy is running and listening on port 17777.\n');

  // Query actual active file after proxy startup (handles startup rotation)
  const targetActive = getLogsStatus().activeFile;
  console.log(`Active Log File to Rotate: ${targetActive}`);

  // 3. Fire requests until active file rotates
  console.log('Firing parallel requests to cross 2 MB threshold (~12,000 requests)...');
  let requestsCompleted = 0;
  let rotated = false;
  let newActive = null;

  const startTime = Date.now();

  async function sendBatch(count) {
    const promises = [];
    for (let i = 0; i < count; i++) {
      promises.push(new Promise((resolve) => {
        const req = http.request(
          `http://127.0.0.1:${PROXY_PORT}/?url=http://127.0.0.1:${MOCK_PORT}/bench`,
          { headers: { Connection: 'close' } },
          (res) => {
            res.resume();
            res.on('end', () => {
              requestsCompleted++;
              resolve();
            });
          }
        );
        req.on('error', () => {
          requestsCompleted++;
          resolve();
        });
        req.end();
      }));
    }
    await Promise.all(promises);
  }

  while (!rotated && requestsCompleted < 30000) {
    await sendBatch(BATCH_SIZE);
    
    // Check status every 250 requests
    if (requestsCompleted % 250 === 0) {
      const currentStatus = getLogsStatus();
      const currentActive = currentStatus.activeFile;
      const activeObj = currentStatus.files.find(f => f.active);
      const activeSize = activeObj ? (activeObj.size / (1024 * 1024)).toFixed(2) : '0';

      process.stdout.write(`\rSent: ${requestsCompleted} reqs | Active: ${currentActive} (${activeSize} MB / 2.00 MB)`);

      if (currentActive && currentActive !== targetActive) {
        rotated = true;
        newActive = currentActive;
        console.log(`\n\n🎉 ROTATION DETECTED! Active marker moved to: ${newActive}`);
        break;
      }
    }
  }

  // Graceful delay for flush
  await sleep(600);

  console.log('\nShutting down proxy gracefully...');
  proxy.kill('SIGINT');
  await sleep(500);
  mockServer.close();

  // 4. Verify Final State
  const finalStatus = getLogsStatus();
  console.log('\n===============================================================');
  console.log('                       FINAL LOGS AUDIT                        ');
  console.log('===============================================================');
  finalStatus.files.forEach(f => {
    console.log(`  - ${f.name.padEnd(20)} : ${(f.size / 1024).toFixed(1)} KB ${f.active ? '⭐ ACTIVE' : '  ARCHIVED'}`);
  });

  const activeCount = finalStatus.files.filter(f => f.active).length;
  console.log('---------------------------------------------------------------');
  console.log(`Active markers found   : ${activeCount} (Expected: exactly 1)`);
  console.log(`Total Requests Sent    : ${requestsCompleted}`);
  console.log(`Elapsed Time           : ${((Date.now() - startTime) / 1000).toFixed(2)}s`);

  if (rotated && activeCount === 1) {
    console.log('\n✅ PASS: Carousel rotation, commit-before-retire & active marker verified!');
  } else {
    console.log('\n❌ FAIL: Rotation did not occur or active marker count is invalid.');
  }
  console.log('===============================================================\n');
});
