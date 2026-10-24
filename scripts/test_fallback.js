#!/usr/bin/env node

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';

const MOCK_PORT = 9099;
const PROXY_PORT = 17777;
const LOGS_DIR = path.resolve('logs');
const BATCH_SIZE = 50;

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
  console.log('         FALLBACK & OVERFLOW CIRCUIT-BREAKER TEST              ');
  console.log('===============================================================');

  const initialStatus = getLogsStatus();
  console.log(`Current Active File : ${initialStatus.activeFile}`);
  console.log('Files currently in logs/:');
  initialStatus.files.forEach(f => {
    console.log(`  - ${f.name.padEnd(20)} : ${(f.size / 1024).toFixed(1)} KB ${f.active ? '(ACTIVE)' : ''}`);
  });

  // 2. Start Proxy
  console.log('\nStarting Proxy Server (./build/api -p 17777)...');
  let stderrAlertDetected = false;
  const proxy = spawn('./build/api', ['-p', String(PROXY_PORT)], {
    stdio: ['ignore', 'pipe', 'pipe']
  });

  proxy.stderr.on('data', data => {
    const msg = data.toString();
    process.stderr.write(`[PROXY STDERR] ${msg}`);
    if (msg.includes('Operating in overflow mode') || msg.includes('Failed to rotate')) {
      stderrAlertDetected = true;
    }
  });

  const ready = await waitForProxy(PROXY_PORT);
  if (!ready) {
    console.error('❌ Failed to connect to proxy on port 17777');
    proxy.kill('SIGINT');
    mockServer.close();
    process.exit(1);
  }
  // Query the actual active file after proxy starts
  const postStartStatus = getLogsStatus();
  const currentActive = postStartStatus.activeFile;
  console.log(`Active Log File after startup: ${currentActive}\n`);

  // 3. Sabotage rotation by making logs/ directory read-only (simulates disk / permission error on rename)
  console.log('[SIMULATION] Setting chmod 555 on logs/ directory to simulate rotation failure...');
  fs.chmodSync(LOGS_DIR, 0o555);

  console.log(`Firing requests to push ${currentActive} past 2 MB limit into overflow mode...`);
  let requestsCompleted = 0;
  let successCount = 0;
  let failCount = 0;

  async function sendBatch(count) {
    const promises = [];
    for (let i = 0; i < count; i++) {
      promises.push(new Promise((resolve) => {
        let done = false;
        const req = http.request(
          `http://127.0.0.1:${PROXY_PORT}/?url=http://127.0.0.1:${MOCK_PORT}/bench`,
          { headers: { Connection: 'close' }, agent: false },
          (res) => {
            res.resume();
            res.on('end', () => {
              if (done) return;
              done = true;
              requestsCompleted++;
              if (res.statusCode === 200) successCount++;
              else failCount++;
              resolve();
            });
          }
        );
        req.on('error', (err) => {
          if (done) return;
          done = true;
          if (failCount < 3) console.log('[REQ ERROR]', err.code || err.message);
          requestsCompleted++;
          failCount++;
          resolve();
        });
        req.end();
      }));
    }
    await Promise.all(promises);
  }

  let crossed2MB = false;

  while (requestsCompleted < 30000) {
    await sendBatch(BATCH_SIZE);

    if (requestsCompleted % 500 === 0) {
      try {
        const activeStat = fs.statSync(path.join(LOGS_DIR, currentActive));
        const sizeMB = (activeStat.size / (1024 * 1024)).toFixed(2);
        process.stdout.write(`\rSent: ${requestsCompleted} reqs | Active File (${currentActive}): ${sizeMB} MB / 2.00 MB`);
        if (activeStat.size > 2 * 1024 * 1024) {
          crossed2MB = true;
          if (stderrAlertDetected) {
            console.log(`\n\n✔ Verified: ${currentActive} crossed 2 MB, rotation failed safely, and proxy entered overflow mode!`);
            break;
          }
        }
      } catch {
        // Ignored
      }
    }
  }

  // Restore permissions before shutting down
  fs.chmodSync(LOGS_DIR, 0o775);
  console.log('\nRestored chmod 775 on logs/ directory.');

  // Let remaining batch complete
  await sleep(1000);
  console.log('Shutting down proxy gracefully...');
  proxy.kill('SIGINT');
  await sleep(500);
  mockServer.close();

  // 4. Verify Final State
  const finalStatus = getLogsStatus();
  console.log('\n===============================================================');
  console.log('                 FALLBACK AUDIT RESULTS                        ');
  console.log('===============================================================');
  finalStatus.files.forEach(f => {
    console.log(`  - ${f.name.padEnd(20)} : ${(f.size / 1024).toFixed(1)} KB ${f.active ? '⭐ ACTIVE' : '  ARCHIVED'}`);
  });

  const activeObj = finalStatus.files.find(f => f.name === currentActive);
  const finalSize = activeObj ? activeObj.size : 0;

  console.log('---------------------------------------------------------------');
  console.log(`Total Requests Sent    : ${requestsCompleted}`);
  console.log(`Successful Requests    : ${successCount}`);
  console.log(`Failed Requests        : ${failCount}`);
  console.log(`Active File Maintained : ${currentActive}`);
  console.log(`Final File Size        : ${(finalSize / (1024 * 1024)).toFixed(2)} MB (> 2.00 MB)`);
  console.log(`Stderr Alert Triggered : ${stderrAlertDetected ? 'YES' : 'NO'}`);

  if (crossed2MB && stderrAlertDetected && failCount === 0 && finalStatus.activeFile === currentActive) {
    console.log('\n✅ PASS: Fallback succeeded! The proxy refused to drop logs, alerted operator,');
    console.log('         and safely operated in overflow mode without crashing or losing requests!');
  } else {
    console.log('\n❌ Check results above.');
  }
  console.log('===============================================================\n');
});
