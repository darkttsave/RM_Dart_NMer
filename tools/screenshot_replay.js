// M2 回放界面截图与交互验证脚本（仅本地验证用）
// 注：M2-UI 后回放页已改为「顶栏 / 视频 / 三页签 / 底部播放控制」结构，DOM 引用已同步。
// 用法: NODE_PATH=$(npm root -g) node tools/screenshot_replay.js <url> <output.png> [width] [height]
const puppeteer = require("puppeteer");

const [, , url, output, widthArg, heightArg] = process.argv;
if (!url || !output) {
    console.error("usage: node screenshot_replay.js <url> <output.png> [width] [height]");
    process.exit(2);
}

const width = Number(widthArg || 1920);
const height = Number(heightArg || 1080);
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: [
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-replay-puppeteer-profile",
            "--window-size=" + width + "," + height,
        ],
    });
    try {
        const errors = [];
        const page = await browser.newPage();
        page.on("console", (msg) => {
            if (msg.type() === "error") errors.push("console: " + msg.text());
        });
        page.on("pageerror", (err) => errors.push("pageerror: " + err.message));
        page.on("requestfailed", (req) => errors.push("requestfailed: " + req.url()));

        await page.setViewport({ width, height, deviceScaleFactor: 1 });
        await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(2500);

        const snapshot = () => page.evaluate(() => ({
            time: document.getElementById("timeText").textContent,
            frameBadge: document.getElementById("frameBadge").textContent,
            sourceFrameBadge: document.getElementById("sourceFrameBadge").textContent,
            frameText: document.getElementById("frameText").textContent,
            frameId: document.getElementById("vFrameId").textContent,
            targetX: document.getElementById("vTargetX").textContent,
            accuracy: document.getElementById("iAccuracy").textContent,
            accuracyTitle: document.getElementById("iAccuracy").title,
            replayMode: document.getElementById("replayModeChip").textContent,
            liveLinkVisible: !document.getElementById("liveLink").hidden,
            liveLinkHref: document.getElementById("liveLink").getAttribute("href"),
            videoNatural: (() => {
                const img = document.getElementById("videoImage");
                return [img.naturalWidth, img.naturalHeight];
            })(),
            chartCards: document.querySelectorAll("#chartsGrid .chart-card").length,
            telemetryMeta: document.getElementById("telemetryMeta").textContent,
            seriesChips: document.querySelectorAll("#seriesChips label").length,
            eventsHeading: document.querySelectorAll(".side .panel-head").length,
            placeholderHidden: document.getElementById("stagePlaceholder").classList.contains("hidden"),
            speed: document.getElementById("speedSelect").value,
            window: document.getElementById("windowSelect").value,
        }));

        const initial = await snapshot();

        // 播放 4 秒
        await page.click("#playBtn");
        await sleep(4000);
        const playing = await snapshot();
        await page.screenshot({ path: output, fullPage: false });

        // 暂停 + 速度切 2x
        await page.click("#playBtn");
        await page.select("#speedSelect", "2");
        await sleep(300);

        // -1s / +1s
        const beforeStep = await page.evaluate(() => document.getElementById("timeText").textContent);
        await page.click("#stepFwdBtn");
        await sleep(1200);
        const afterFwd = await page.evaluate(() => document.getElementById("timeText").textContent);
        await page.click("#stepBackBtn");
        await sleep(1200);
        const afterBack = await page.evaluate(() => document.getElementById("timeText").textContent);

        // 按帧号定位
        await page.click("#frameIdInput", { clickCount: 3 });
        await page.type("#frameIdInput", "100");
        await page.click("#locateBtn");
        await sleep(1500);
        const located = await snapshot();
        const locateResult = await page.evaluate(() => document.getElementById("locateResult").textContent);

        // 曲线窗口切 5s + 展开字段选择并取消一个字段
        await page.select("#windowSelect", "5");
        await sleep(800);
        const window5 = await snapshot();
        await page.click(".series-bar summary");
        await sleep(400);
        const firstCheckbox = await page.$("#seriesChips input[type=checkbox]");
        if (firstCheckbox) {
            await firstCheckbox.click();
            await sleep(800);
        }
        const afterUncheck = await snapshot();

        // 全屏切换（页面内/浏览器全屏任一即可）
        await page.click("#fsBtn");
        await sleep(600);
        const fullscreen = await page.evaluate(() => ({
            label: document.getElementById("fsBtn").textContent,
            faux: document.getElementById("videoPanel").classList.contains("faux-fullscreen"),
            real: !!document.fullscreenElement,
        }));
        await page.keyboard.press("Escape");
        await sleep(500);

        console.log(JSON.stringify({
            errors, initial, playing, steps: { beforeStep, afterFwd, afterBack },
            located, locateResult, window5, afterUncheck, fullscreen,
        }, null, 2));
    } finally {
        await browser.close();
    }
})().catch((err) => {
    console.error("screenshot failed:", err);
    process.exit(1);
});
