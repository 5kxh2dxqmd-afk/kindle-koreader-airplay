(function () {
    var API = "http://127.0.0.1:7080";
    var splash, viewer, errorPage, statusText, statusDot, frame, frameNote;
    var connected = false;
    var failures = 0;
    var lastFrameId = -1;

    function byId(id) { return document.getElementById(id); }

    function request(path, done) {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", API + path, true);
        xhr.onreadystatechange = function () {
            if (xhr.readyState === 4) {
                if (xhr.status >= 200 && xhr.status < 300) done(null, xhr);
                else done(new Error("HTTP " + xhr.status));
            }
        };
        xhr.onerror = function () { done(new Error("Network error")); };
        xhr.send(null);
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
        request("/api/status?t=" + new Date().getTime(), function (err, xhr) {
            if (err) {
                failures++;
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
            catch (parseError) { window.setTimeout(update, 1500); return; }

            if (data.connected) {
                connected = true;
                showViewer();
                if (data.hasFrame) {
                    frameNote.innerHTML = "";
                    if (data.frameId !== lastFrameId) {
                        lastFrameId = data.frameId;
                        frame.src = API + "/api/frame.bmp?t=" + new Date().getTime();
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

    document.addEventListener("DOMContentLoaded", function () {
        splash = byId("splash");
        viewer = byId("viewer");
        errorPage = byId("error");
        statusText = byId("status-text");
        statusDot = byId("status-dot");
        frame = byId("airplay-frame");
        frameNote = byId("frame-note");
        frame.onerror = function () { frameNote.innerHTML = "Waiting for picture…"; };
        update();
    });
}());
