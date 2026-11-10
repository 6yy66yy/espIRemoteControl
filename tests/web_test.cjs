const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { chromium } = require('playwright');

const source = fs.readFileSync(path.join(__dirname, '../ir_receive_demo/web_page.h'), 'utf8');
const html = source.split('R"ACPAGE(')[1].split(')ACPAGE";')[0];
const output = path.join(os.tmpdir(), 'espIRemoteControl-tests');
fs.mkdirSync(output, { recursive: true });
let rules = { version: 1, rules: [
  { id: 1, enabled: true, direction: 'rising', thresholdC: 28, windowC: 2, action: 'cool', targetC: 26 },
  { id: 2, enabled: true, direction: 'falling', thresholdC: 24, windowC: 2, action: 'off', targetC: 24 }
] };
let automatic = true;
let failStatus = false;
let power = 'off';
let mode = 'cool';
let targetC = 26;
let failSave = false;
const commands = [];
function status() {
  return { automatic, cloudConnected: true, ip: '192.168.2.192', rssi: -32,
    uptimeMs: 60000, freeHeap: 148000, minFreeHeap: 140000,
    sensor: { valid: true, temperatureC: 23.2, humidity: 38.2, ageMs: 800 },
    ac: { power, mode, targetC }, rules: rules.rules.map(rule => ({ id: rule.id, waiting: rule.id === 2 })),
    lastRule: { id: 2, action: 'off', targetC: 24, sent: true, ageMs: 4000 } };
}
const server = http.createServer(async (req, res) => {
  let text = '';
  for await (const chunk of req) text += chunk;
  res.setHeader('Content-Type', req.url === '/' ? 'text/html; charset=utf-8' : 'application/json');
  if (req.url === '/') return res.end(html);
  if (req.url === '/api/status') {
    if (failStatus) { res.statusCode = 503; return res.end('{"error":"offline"}'); }
    return res.end(JSON.stringify(status()));
  }
  if (req.url === '/api/control') {
    const { command } = JSON.parse(text);
    commands.push(command);
    if (command === 'AUTO') automatic = true;
    else { automatic = false; if (command === 'OFF') power = 'off'; }
    return res.end(JSON.stringify(status()));
  }
  if (req.url === '/api/rules') {
    if (req.method === 'PUT') {
      if (failSave) { res.statusCode = 500; return res.end('{"error":"保存失败，原规则未修改"}'); }
      rules = JSON.parse(text);
    }
    return res.end(JSON.stringify(rules));
  }
  res.statusCode = 404;
  res.end('{}');
});

async function verifyLayout(page) {
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false,
    'page must not scroll horizontally');
  const overlaps = await page.evaluate(() => {
    const issues = [];
    for (const row of document.querySelectorAll('.rule')) {
      const children = [...row.children];
      for (let i = 0; i < children.length; i++) for (let j = i + 1; j < children.length; j++) {
        const a = children[i].getBoundingClientRect(), b = children[j].getBoundingClientRect();
        if (Math.min(a.right, b.right) - Math.max(a.left, b.left) > 1 &&
            Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > 1) issues.push([i, j]);
      }
    }
    return issues;
  });
  assert.deepEqual(overlaps, [], 'rule cells must not overlap');
}

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ headless: true, channel: process.env.PLAYWRIGHT_CHANNEL || 'msedge' });
  const url = `http://127.0.0.1:${server.address().port}`;
  try {
    for (const viewport of [{ width: 1440, height: 1000 }, { width: 900, height: 1000 }, { width: 390, height: 844 }, { width: 320, height: 740 }]) {
      const context = await browser.newContext({ viewport });
      const page = await context.newPage();
      const errors = [];
      page.on('pageerror', error => errors.push(error.message));
      await page.goto(url);
      await page.waitForFunction(() => document.querySelectorAll('.rule').length === 2 && document.getElementById('temperature').textContent.includes('23.2'));
      await verifyLayout(page);
      await page.screenshot({ path: path.join(output, `web-${viewport.width}.png`), fullPage: true });
      assert.deepEqual(errors, [], 'no browser errors');
      await context.close();
    }
    const page = await browser.newPage({ viewport: { width: 1280, height: 1000 } });
    await page.goto(url);
    await page.waitForFunction(() => document.querySelectorAll('.rule').length === 2);
    await page.locator('#add').click();
    const row = page.locator('.rule').last();
    await row.locator('[aria-label="触发温度"]').fill('22');
    await row.locator('[aria-label="复位窗口"]').fill('1');
    assert.match(await row.locator('.reset').textContent(), /23.0/);
    await row.locator('[aria-label="上移规则"]').click();
    assert.equal(await page.locator('.rule').nth(1).getAttribute('data-id'), '3');
    await page.locator('.rule').nth(1).locator('[aria-label="启用规则 3"]').uncheck();
    await page.locator('#save').click();
    await page.waitForFunction(() => document.getElementById('rules-notice').textContent === '规则已保存');
    assert.equal(rules.rules[1].enabled, false);
    assert.equal(rules.rules[1].windowC, 1);
    await page.locator('.rule').nth(1).locator('[aria-label="触发温度"]').fill('22.05');
    await page.locator('#save').click();
    assert.match(await page.locator('#rules-notice').textContent(), /最多一位小数/);
    await page.locator('.rule').nth(1).locator('[aria-label="触发温度"]').fill('21');
    failSave = true;
    await page.locator('#save').click();
    await page.waitForFunction(() => document.getElementById('rules-notice').textContent.includes('保存失败'));
    assert.equal(rules.rules[1].thresholdC, 22, 'failed save retains previous config');
    failSave = false;
    await page.locator('.rule').nth(1).locator('[aria-label="删除规则"]').click();
    for (let i = 0; i < 8; i++) await page.locator('#add').click();
    assert.equal(await page.locator('.rule').count(), 10);
    assert.equal(await page.locator('#add').isDisabled(), true);
    await verifyLayout(page);
    await page.locator('#off').click();
    await page.waitForFunction(() => document.getElementById('control-mode').textContent === '手动');
    await page.locator('#auto').click();
    await page.waitForFunction(() => document.getElementById('control-mode').textContent === '自动');
    await page.locator('[name=mode][value=heat]').check();
    await page.locator('#target').fill('22');
    await page.locator('#start').click();
    await page.waitForFunction(() => document.getElementById('control-notice').textContent === '指令已发送');
    assert(commands.includes('HEAT 22'));
    failStatus = true;
    await page.waitForFunction(() => document.getElementById('connection').textContent === '设备连接中断');
    assert.equal(await page.locator('#temperature').textContent(), '-- °C');
    console.log(`PASS: responsive layouts, rule editing, validation, save failures, ten-rule limit, manual/AUTO controls, offline state; screenshots: ${output}`);
  } finally {
    await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
