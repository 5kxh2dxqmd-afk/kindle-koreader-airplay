(function () {
    /* v2: on-screen debug strip + several start triggers + XHR watchdog.
     * Status stays on the local receiver; the picture is a file next to this
     * WAF (current.bmp). */
    var API = "http://127.0.0.1:7080";
    var DEBUG = false;   /* true = show status strip at bottom of the screen */
    var splash, viewer, errorPage, statusText, statusDot, frame, frameNote, dbgEl;
    var connected = false, failures = 0, lastFrameId = -1, started = false;
    var polls = 0, lastInfo = "booting", jsErrors = "", imgInfo = "img:-";

    function byId(id) { return document.getElementById(id); }

    function paint() {
        if (!DEBUG) return;
        if (!dbgEl && document.body) {
            dbgEl = document.createElement("div");
            dbgEl.style.cssText = "position:fixed;left:0;bottom:0;z-index:99;width:100%;" +
                "padding:2px 6px;font:11px monospace;background:#fff;color:#000;border-top:1px solid #000;";
            document.body.appendChild(dbgEl);
        }
        if (dbgEl) {
            dbgEl.textContent = "v3 js:ok polls:" + polls + " fail:" + failures + " | " + lastInfo + " | " + imgInfo +
                (jsErrors ? " | JSERR " + jsErrors : "");
        }
    }
    function dbg(msg) { lastInfo = msg; paint(); }

    window.onerror = function (m, u, l) { jsErrors = m + "@" + l; paint(); return false; };

    function request(path, done) {
        var xhr = new XMLHttpRequest();
        var finished = false;
        function finish(err) { if (finished) return; finished = true; done(err, xhr); }
        try { xhr.open("GET", API + path, true); }
        catch (e) { finish(new Error("open threw " + e)); return; }
        xhr.onreadystatechange = function () {
            if (xhr.readyState === 4) {
                if (xhr.status >= 200 && xhr.status < 300) finish(null);
                else finish(new Error("HTTP " + xhr.status));
            }
        };
        xhr.onerror = function () { finish(new Error("xhr.onerror")); };
        try { xhr.timeout = 4000; xhr.ontimeout = function () { finish(new Error("timeout")); }; } catch (t) {}
        window.setTimeout(function () {
            if (!finished) {
                var rs = xhr.readyState;
                try { xhr.abort(); } catch (a) {}
                finish(new Error("no reply in 5s rs=" + rs));
            }
        }, 5000);
        try { xhr.send(null); } catch (e2) { finish(new Error("send threw " + e2)); }
    }

    function showSplash(message) {
        splash.className = "screen";
        viewer.className = "screen hidden";
        errorPage.className = "screen hidden";
        statusText.innerHTML = message || "Ready to connect";
        statusDot.style.background = connected ? "#111" : "#777";
    }

    function showViewer() {
        splash.className = "screen hidden";
        viewer.className = "screen";
        errorPage.className = "screen hidden";
    }

    function update() {
        polls++;
        request("/api/status?t=" + new Date().getTime(), function (err, xhr) {
            if (err) {
                failures++;
                dbg("ERR " + err.message);
                if (failures > 3) {
                    splash.className = "screen hidden";
                    viewer.className = "screen hidden";
                    errorPage.className = "screen";
                }
                window.setTimeout(update, 1500);
                return;
            }

            failures = 0;
            var data;
            try { data = JSON.parse(xhr.responseText); }
            catch (parseError) { dbg("bad JSON: " + xhr.responseText); window.setTimeout(update, 1500); return; }

            dbg("HTTP ok conn=" + data.connected + " frame=" + data.hasFrame + " id=" + data.frameId);
            if (data.connected) {
                connected = true;
                showViewer();
                if (data.hasFrame) {
                    frameNote.innerHTML = "";
                    if (data.frameId !== lastFrameId) {
                        lastFrameId = data.frameId;
                        frame.src = "current.bmp?t=" + data.frameId;
                    }
                } else {
                    frameNote.innerHTML = "Waiting for the first picture…";
                }
            } else {
                connected = false;
                showSplash("Ready to connect");
                frame.removeAttribute("src");
                lastFrameId = -1;
            }
            window.setTimeout(update, 1400);
        });
    }

    function start() {
        if (started) return;
        splash = byId("splash"); viewer = byId("viewer"); errorPage = byId("error");
        statusText = byId("status-text"); statusDot = byId("status-dot");
        frame = byId("airplay-frame"); frameNote = byId("frame-note");
        if (!splash || !frame) { return; }
        started = true;
        frame.onload = function () { imgInfo = "img ok " + frame.naturalWidth + "x" + frame.naturalHeight; paint(); };
        frame.onerror = function () {
            if (lastFrameId >= 0 && frame.src.indexOf("current.bmp") >= 0) {
                imgInfo = "img ERR file, trying http"; paint();
                frame.src = API + "/api/frame.bmp?t=" + lastFrameId;
                return;
            }
            imgInfo = "img ERR http"; paint();
            frameNote.innerHTML = "Waiting for picture…";
        };
        paint();
        update();
    }

    if (document.addEventListener) {
        document.addEventListener("DOMContentLoaded", start, false);
        window.addEventListener("load", start, false);
    }
    window.setTimeout(start, 800);
    window.setTimeout(start, 2500);
}());
