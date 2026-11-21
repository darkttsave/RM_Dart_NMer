// M3 界面验证脚本（仅本地验证用）：参数调整页签 + 回放参数事件页签
// 用法: NODE_PATH=$(npm root -g) node tools/regression_m3.js [liveUrl] [replayUrl] [outPrefix] [sessionName]
const puppeteer = require("puppeteer");

const LIVE_URL = process.argv[2] || "http://127.0.0.1:5000/";
const REPLAY_URL = process.argv[3] || "http://127.0.0.1:5001/";
const OUT = process.argv[4] || "/tmp/m3ui";
const SESSION_HINT = process.argv[5] || "";
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function collectErrors(page, errors) {
    page.on("console", (msg) => {
        if (msg.type() === "error") errors.push("console: " + msg.text());
    });
    page.on("pageerror", (err) => errors.push("pageerror: " + err.message));
    page.on("requestfailed", (req) => {
        const target = req.url();
        if (target.includes("/video_feed") || target.includes("/api/telemetry/stream")) return;
        errors.push("requestfailed: " + target);
    });
}

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: [
            "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-m3-puppeteer-profile", "--window-size=1920,1080",
        ],
    });
    const report = { checks: {}, live: {}, replay: {}, errors: [] };
    const ok = (name, pass, detail) => { report.checks[name] = { pass: !!pass, detail }; };

    try {
        // ==================== A. 实时页参数调整 ====================
        const live = await browser.newPage();
        collectErrors(live, report.errors);
        await live.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await live.goto(LIVE_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(4000);
        await live.click("#tabBtnParams");
        await sleep(2000);

        const paramsInfo = await live.evaluate(() => ({
            rows: document.querySelectorAll("#paramGroups .param-row").length,
            panels: document.querySelectorAll("#paramGroups .panel").length,
            message: document.getElementById("paramMessage").textContent,
            state: document.getElementById("paramState").textContent,
            bright: (() => {
                const row = document.querySelector('#paramGroups .param-row[data-name="bright_thresh"]');
                return row ? row.querySelector(".param-state").textContent : null;
            })(),
            applyDisabled: document.getElementById("paramApplyBtn").disabled,
            saveDisabled: document.getElementById("paramSaveBtn").disabled,
        }));
        ok("参数页签渲染", paramsInfo.rows >= 15 && paramsInfo.panels >= 5,
            `${paramsInfo.panels} 组 / ${paramsInfo.rows} 项`);
        ok("运行值与配置值展示", !!paramsInfo.bright && paramsInfo.bright.includes("/"),
            paramsInfo.bright);

        // 修改 brightness -> 应用
        const newBright = await live.evaluate(() => {
            const input = document.querySelector('#paramGroups input[data-name="bright_thresh"]');
            const cur = Number(input.value);
            const next = cur === 105 ? 110 : 105;
            input.value = String(next);
            input.dispatchEvent(new Event("input", { bubbles: true }));
            return next;
        });
        await sleep(400);
        const afterEdit = await live.evaluate(() => ({
            message: document.getElementById("paramMessage").textContent,
            applyDisabled: document.getElementById("paramApplyBtn").disabled,
        }));
        ok("本地修改识别", afterEdit.message.includes("未应用修改") && !afterEdit.applyDisabled,
            afterEdit.message);

        await live.click("#paramApplyBtn");
        await sleep(2000);
        const afterApply = await live.evaluate(() => ({
            message: document.getElementById("paramMessage").textContent,
            state: (() => {
                const row = document.querySelector('#paramGroups .param-row[data-name="bright_thresh"]');
                return row ? row.querySelector(".param-state").textContent : null;
            })(),
            saveDisabled: document.getElementById("paramSaveBtn").disabled,
        }));
        ok("应用参数（热更新）", afterApply.message.includes("已应用"),
            `${afterApply.message} | 状态列 ${afterApply.state}`);
        await live.screenshot({ path: OUT + "_live_params.png" });

        await live.click("#paramSaveBtn");
        await sleep(2500);
        const afterSave = await live.evaluate(() => ({
            message: document.getElementById("paramMessage").textContent,
            saveDisabled: document.getElementById("paramSaveBtn").disabled,
        }));
        ok("保存配置", afterSave.message.includes("已保存") || afterSave.message.includes("配置已保存"),
            afterSave.message);

        // 切回状态页签确认原页面不受影响
        await live.click("#tabBtnStatus");
        await sleep(600);
        const backStatus = await live.evaluate(() => ({
            visible: !document.getElementById("tabStatus").hidden,
            detect: document.getElementById("detectState").textContent,
            paramsHidden: document.getElementById("tabParams").hidden,
        }));
        ok("页签切换回归", backStatus.visible && backStatus.paramsHidden && backStatus.detect.length > 0,
            JSON.stringify(backStatus));

        report.live = { paramsInfo, afterEdit, afterApply, afterSave, backStatus };

        // ==================== B. 回放页参数事件 ====================
        const replay = await browser.newPage();
        collectErrors(replay, report.errors);
        await replay.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await replay.goto(REPLAY_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(3000);

        if (SESSION_HINT) {
            await replay.evaluate((hint) => {
                const select = document.getElementById("sessionSelect");
                const opt = Array.from(select.options).find((o) => o.textContent.includes(hint));
                if (opt) {
                    select.value = opt.value;
                    select.dispatchEvent(new Event("change", { bubbles: true }));
                }
            }, SESSION_HINT);
            await sleep(2500);
        }

        await replay.click("#tabBtnEvents");
        await sleep(800);
        const eventsInfo = await replay.evaluate(() => ({
            items: document.querySelectorAll("#eventList .event-item").length,
            meta: document.getElementById("eventMeta").textContent,
            empty: (document.querySelector("#eventList .events-empty") || {}).textContent || null,
            session: document.getElementById("iSession").textContent,
        }));
        ok("参数事件页签", eventsInfo.items > 0 || !!eventsInfo.empty,
            `${eventsInfo.items} 条 / ${eventsInfo.meta} / 空状态=${eventsInfo.empty}`);
        ok("事件列表内容", eventsInfo.items === 0 || eventsInfo.meta.includes(String(eventsInfo.items)),
            eventsInfo.meta);

        const beforeJump = await replay.evaluate(() => document.getElementById("timeText").textContent);
        let afterJump = null;
        if (eventsInfo.items > 0) {
            await replay.click("#eventList .event-item");
            await sleep(1200);
            afterJump = await replay.evaluate(() => ({
                time: document.getElementById("timeText").textContent,
                locate: document.getElementById("locateResult").textContent,
                frame: document.getElementById("frameText").textContent,
                active: document.querySelectorAll("#eventList .event-item.active").length,
                videoLoaded: (() => {
                    const img = document.getElementById("videoImage");
                    return !!(img && img.complete && img.naturalWidth > 0);
                })(),
            }));
            ok("点击事件跳转", beforeJump !== afterJump.time && afterJump.locate.includes("已跳转"),
                `${beforeJump} -> ${afterJump.time} | ${afterJump.locate}`);
            ok("跳转后视频定位", afterJump.videoLoaded && afterJump.frame !== "帧 --", afterJump.frame);
        }
        await replay.screenshot({ path: OUT + "_replay_events.png" });

        // 会话信息页签显示事件统计
        await replay.click("#tabBtnSession");
        await sleep(600);
        const sessionInfo = await replay.evaluate(() => ({
            iEvents: document.getElementById("iEvents").textContent,
            accuracy: document.getElementById("iAccuracy").textContent,
        }));
        ok("会话信息事件统计", /条|events|无/.test(sessionInfo.iEvents), sessionInfo.iEvents);

        report.replay = { eventsInfo, beforeJump, afterJump, sessionInfo };

        report.summary = {
            total: Object.keys(report.checks).length,
            passed: Object.values(report.checks).filter((c) => c.pass).length,
            failed: Object.entries(report.checks).filter(([, c]) => !c.pass).map(([k]) => k),
            js_errors: report.errors,
        };
        console.log(JSON.stringify(report, null, 2));
    } finally {
        await browser.close();
    }
})().catch((err) => {
    console.error("m3 ui test failed:", err);
    process.exit(1);
});
