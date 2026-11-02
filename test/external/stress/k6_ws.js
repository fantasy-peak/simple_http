// k6 WebSocket echo load script (open-source tool, same on both servers).
// Each iteration opens ONE connection to /echo, sends WANT frames back-to-back
// on open, counts echoes, closes when WANT reached (or on the safety timeout).
// msgs/s = ws_messages counter total / wall time (k6 reports both).
import ws from 'k6/ws';
import { Counter } from 'k6/metrics';

const msgsMetric = new Counter('ws_messages');

export const options = {
  vus: __ENV.VUS ? Number(__ENV.VUS) : 100,
  iterations: __ENV.ITERS ? Number(__ENV.ITERS) : 20,
};

const WANT = __ENV.WANT ? Number(__ENV.WANT) : 1000;
const SIZE = __ENV.SIZE ? Number(__ENV.SIZE) : 512;
const URI = __ENV.WS_URL || 'ws://127.0.0.1:7788/echo';

export default function () {
  ws.connect(URI, null, function (socket) {
    let received = 0;
    socket.setTimeout(function () {
      // safety: if we never reached WANT, close anyway; the counter below
      // tells the truth about how many echoes arrived.
      socket.close();
    }, 20000);
    socket.on('open', function () {
      const payload = 'x'.repeat(SIZE);
      for (let i = 0; i < WANT; i++) {
        socket.send(payload);
      }
    });
    socket.on('message', function () {
      received++;
      msgsMetric.add(1);
      if (received === WANT) {
        socket.close();
      }
    });
    socket.on('error', function (e) {
      socket.close();
    });
  });
}