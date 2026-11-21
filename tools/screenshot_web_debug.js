// M1 Web 调试界面截图脚本（仅本地验证用，不进入交付代码）
// 注：M2-UI 后页面已改为「顶栏 / 视频 / 页签 / 底栏」结构，本脚本的 DOM 引用已同步。
// 用法: NODE_PATH=$(npm root -g) node tools/screenshot_web_debug.js <url> <output.png> [width] [height] [waitMs]
const puppeteer = require("puppeteer");

const [, , url, output, widthArg, heightArg, waitArg] = process.argv;
if (!url || !output) {
    console.error("usage: node screenshot_web_debug.js <url> <output.png> [width] [height] [waitMs]");
    process.exit(2);
}

const width = Number(widthArg || 1920);
const height = Number(heightArg || 1080);
const waitMs = Number(waitArg || 12000);
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: [
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-web-debug-puppeteer-profile",
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
        page.on("requestfailed", (req) => {
            if (!req.url().includes("/video_feed") && !req.url().includes("/api/telemetry/stream")) {
                errors.push("requestfailed: " + req.url());
            }
        });

        await page.setViewport({ width, height, deviceScaleFactor: 1 });
        await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });
        await new Promise((resolve) => setTimeout(resolve, waitMs));
        await page.screenshot({ path: output, fullPage: false });

        const metrics = await page.evaluate(() => {
            const text = (id) => {
                const el = document.getElementById(id);
                return el ? el.textContent.trim() : null;
            };
            const canvases = Array.from(document.querySelectorAll(".chart-card canvas")).map((c) => ({
                id: c.id,
                width: c.width,
                height: c.height,
                drawn: c.width > 0 && c.height > 0,
            }));
            return {
                link: text("linkText"),
                source: text("sourceChip"),
                detect: text("detectState"),
                targetX: text("vTargetX"),
                targetY: text("vTargetY"),
                turn: text("vTurn"),
                distance: text("vDistance"),
                clear: text("vClear"),
                txSent: text("vTxSent"),
                perfLine: text("perfNote"),
                receiver: text("boLink"),
                seq: text("footSeq"),
                canvases,
                videoComplete: (() => {
                    const img = document.getElementById("videoImage");
                    return img ? img.complete && img.naturalWidth > 0 : false;
                })(),
                videoNaturalSize: (() => {
                    const img = document.getElementById("videoImage");
                    return img ? [img.naturalWidth, img.naturalHeight] : null;
                })(),
            };
        });
        console.log(JSON.stringify({ errors, metrics }, null, 2));
    } finally {
        await browser.close();
    }
})().catch((err) => {
    console.error("screenshot failed:", err);
    process.exit(1);
});
