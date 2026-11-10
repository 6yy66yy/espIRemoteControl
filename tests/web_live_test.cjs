const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const { chromium } = require('playwright');

const url = process.argv[2];
if (!url) throw Error('Usage: node tests/web_live_test.cjs http://device-IP');
const output = path.join(os.tmpdir(), 'espIRemoteControl-tests');
fs.mkdirSync(output, { recursive: true });

(async () => {
  const browser = await chromium.launch({ headless: true, channel: process.env.PLAYWRIGHT_CHANNEL || 'msedge' });
  try {
    for (const viewport of [{ width: 1440, height: 1000 }, { width: 390, height: 844 }]) {
      const page = await browser.newPage({ viewport });
      const errors = [];
      page.on('pageerror', error => errors.push(error.message));
      await page.goto(url);
      await page.waitForFunction(() => document.getElementById('connection').textContent === '设备在线' &&
        document.getElementById('rule-count').textContent !== '0 / 10');
      assert.match(await page.locator('#temperature').textContent(), /\d+\.\d °C/);
      assert.equal(await page.locator('#cloud-state').textContent(), '巴法云：已连接');
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
      await page.screenshot({ path: path.join(output, `device-${viewport.width}.png`), fullPage: true });
      assert.deepEqual(errors, []);
      console.log(`PASS: device web page at ${viewport.width}px; ${await page.locator('#memory').textContent()}`);
      await page.close();
    }
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
