const puppeteer = require('puppeteer');
(async()=>{
 const browser=await puppeteer.launch({headless:true,executablePath:process.env.CHROME_PATH || `${process.env.HOME}/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome`,args:['--no-sandbox','--disable-setuid-sandbox']});
 try {
  const p=await browser.newPage(); await p.setViewport({width:1440,height:900});
  await p.goto(process.argv[2],{waitUntil:'domcontentloaded'});
  await p.waitForFunction(()=>document.querySelector('#dartMode').textContent.includes('红色'),{timeout:15000});
  await p.click('#tabBtnParams');
  await p.waitForSelector('select[data-name="dart_color"]');
  await p.select('select[data-name="dart_color"]','1');
  await p.click('#paramApplyBtn');
  await p.waitForFunction(()=>document.querySelector('#paramMessage').textContent.includes('已应用'));
  await p.waitForFunction(()=>document.querySelector('#dartMode').textContent.includes('蓝色'));
  await p.click('#tabBtnStatus');
  await p.click('#dartResetBtn');
  await p.waitForFunction(()=>document.querySelector('#dartResetMessage').textContent.includes('下一帧生效'));
  console.log('PASS 浏览器飞镖颜色选择、立即生效、状态及重置反馈');
 } finally {await browser.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
