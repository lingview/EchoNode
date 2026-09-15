// 截图面板：task_req screenshot → binary(0x02, JPEG) 拼装 → 汇总校验 → 显示
const Shots = {
  shotTaskId: "",
  chunks: [],
  sha: "",

  capture(agentId) {
    this.shotTaskId = "";
    this.chunks = [];
    this.sha = "";
    Ws.sendJson({ type: "task_req", agentId: agentId, action: "screenshot", payload: {} });
    $("shot-status").textContent = "截图中...";
  },

  onMessage(msg) {
    if (msg.type !== "task_result" ||
        !msg.data || msg.data.indexOf("\"sha256\"") === -1) return false;
    try {
      const summary = JSON.parse(msg.data);
      this.sha = summary.sha256 || "";
      if (!this.chunks.length) {
        $("shot-status").textContent = "截图失败: 未收到图像数据";
        return true;
      }
      this._finish();
    } catch (e) {
      $("shot-status").textContent = "截图失败: " + (msg.error || "未知错误");
    }
    return true;
  },

  // 流类型 0x02 的 binary 帧
  onBinary(frame) {
    if (frame.byteLength < 21) return false;
    const u8 = new Uint8Array(frame);
    if (u8[20] !== 0x02) return false;
    // taskId 由 server 生成、前端无从预知；同会话内 0x02 流只属于本次截图
    const seq = (u8[16] << 24) | (u8[17] << 16) | (u8[18] << 8) | u8[19];
    this.chunks.push({ seq, data: u8.slice(21) });
    return true;
  },

  _finish() {
    this.chunks.sort((a, b) => a.seq - b.seq);
    let total = 0;
    for (const c of this.chunks) total += c.data.length;
    const merged = new Uint8Array(total);
    let off = 0;
    for (const c of this.chunks) { merged.set(c.data, off); off += c.data.length; }
    this.chunks = [];

    crypto.subtle.digest("SHA-256", merged).then(hashBuf => {
      const sha = [...new Uint8Array(hashBuf)]
        .map(b => b.toString(16).padStart(2, "0")).join("");
      if (this.sha && sha !== this.sha) {
        $("shot-status").textContent = "截图校验失败(sha256 不符)";
        return;
      }
      const url = URL.createObjectURL(new Blob([merged], { type: "image/jpeg" }));
      const img = $("shot-image");
      img.src = url;
      img.classList.remove("hidden");
      $("shot-status").textContent = `截图完成 (${total}B)`;
    });
  }
};

// 任务历史面板：GET /api/tasks 最近 200 条
const History = {
  async refresh() {
    if (!currentAgentId) return;
    const resp = await Api.request("/api/tasks?agentId=" + encodeURIComponent(currentAgentId));
    const list = await resp.json();
    const tbody = $("tasks-table").querySelector("tbody");
    tbody.innerHTML = "";
    for (const t of (list || []).slice(0, 50)) {
      const tr = document.createElement("tr");
      const time = t.createdAt ? new Date(t.createdAt * 1000).toLocaleString() : "-";
      const result = (t.result || "").slice(0, 60);
      tr.innerHTML = `<td>${time}</td><td>${t.action}</td>` +
        `<td class="status-${t.status}">${t.status}</td><td title="${t.result || ""}">${result}</td>`;
      tbody.appendChild(tr);
    }
  }
};

// 系统信息面板：定时轮询 agent 的 systeminfo 命令
const SysInfo = {
  pending: false,
  lastAt: 0,

  refresh() {
    if (!currentAgentId) return;
    const now = Date.now();
    if (this.pending && now - this.lastAt < 20000) return; // 上一次还没回，不重复发
    if (!this.pending && now - this.lastAt < 3000) return;  // 节流
    this.pending = true;
    this.lastAt = now;
    Ws.sendJson({ type: 'task_req', agentId: currentAgentId,
                  action: 'systeminfo', payload: { command: 'systeminfo' } });
  },

  onMessage(msg) {
    if (msg.type !== 'task_result' || msg.action !== 'systeminfo') return false;
    this.pending = false;
    const body = $("sysinfo-body");
    if (msg.ok) {
      body.textContent = (msg.data || '').slice(0, 3000) || '（无输出）';
    } else {
      body.textContent = '获取失败: ' + (msg.error || '');
    }
    return true;
  }
};
