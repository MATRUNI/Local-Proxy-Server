const http = require('http');

const port = parseInt(process.env.MOCK_PORT || '9099', 10);

http.createServer((req, res) => {
  res.writeHead(200, {
    'Content-Type': 'application/json',
    'Content-Length': '27',
    'Connection': 'keep-alive'
  });
  res.end('{"status":"ok","speed":"c"}');
}).listen(port, '127.0.0.1');

