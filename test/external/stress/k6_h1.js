// k6 压力驱动：HTTP/1.1 档（替代 h2load）。
//
// 请求：POST 1 KiB body（与需求方的请求包规格一致）；响应体由 `?n=` 指定。
// 门线：errors（状态 != 200）必须为 0。
//
// 环境变量：
//   K6_URL         完整 URL，含 ?n=（默认 http://127.0.0.1:7791/world?n=2048）
//   K6_VUS         VU 数（默认 100）
//   K6_ITERATIONS  总请求数（默认 200000，与 h2load 档一致）
import http from 'k6/http';
import { Counter } from 'k6/metrics';

const body = 'x'.repeat(1024);
const url = __ENV.K6_URL || 'http://127.0.0.1:7791/world?n=2048';
const vus = Number(__ENV.K6_VUS || 100);
const iters = Number(__ENV.K6_ITERATIONS || 200000);

export const options = {
  vus: vus,
  iterations: iters,
  discardResponseBodies: true,
};

const errors = new Counter('errors');

export default function () {
  const res = http.post(url, body, { headers: { 'Content-Type': 'application/octet-stream' } });
  if (res.status !== 200) {
    errors.add(1);
  }
}