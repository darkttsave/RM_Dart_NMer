// 需要 puppeteer 和本地 Chromium；传入正在运行的视觉调试页面 URL。
const puppeteer = require('puppeteer');
(async () => {
  const browser = await puppeteer.launch({
    headless: true,
    executablePath: process.env.CHROME_PATH || `${process.env.HOME}/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome`,
    args: ['--no-sandbox', '--disable-setuid-sandbox'],
  });
  try {
    const page = await browser.newPage();
    await page.setViewport({ width: 1440, height: 900 });
    await page.goto(process.argv[2], { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => !document.querySelector('#recordToggleBtn').disabled &&
      document.querySelector('#recordToggleBtn').textContent === '开始录制');
    if (await page.$eval('#recordImageMode', el => el.value) !== 'debug') throw new Error('默认模式错误');
    await page.select('#recordImageMode', 'raw');
    await page.click('#recordToggleBtn');
    await page.waitForFunction(() => document.querySelector('#recordImageMode').disabled &&
      document.querySelector('#recordChip').textContent.includes('原始画面'));
    await page.click('#recordToggleBtn');
    await page.waitForFunction(() => !document.querySelector('#recordImageMode').disabled &&
      document.querySelector('#recordToggleBtn').textContent === '开始录制');
    await page.select('#recordImageMode', 'debug');
    await page.click('#recordToggleBtn');
    await page.waitForFunction(() => document.querySelector('#recordImageMode').disabled &&
      document.querySelector('#recordChip').textContent.includes('调试画面'));
    await page.click('#recordToggleBtn');
    await page.waitForFunction(() => !document.querySelector('#recordImageMode').disabled);
    console.log('PASS UI: 默认 debug、选择 raw、录制锁定、停止后切换 debug');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
