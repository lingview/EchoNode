const DesktopPanel = {
  streams: {},
  cursorShape: null,
  active: "",
  canvas: null, ctx: null,

  fps() {
    const el = $("desk-fps");
    return el ? Number(el.value) : 30;
  },

  start(agentId) {
    Ws.sendJson({ type: "task_req", agentId: agentId, action: "remote_start",
                  payload: { fps: this.fps(), quality: 60 } });
    $("desk-status").textContent = "正在连接远程桌面...";
  },

  stop(agentId) {
    Ws.sendJson({ type: "task_req", agentId: agentId, action: "remote_stop",
                  payload: {} });
    this.streams[agentId] = null;
    if (this.active === agentId) {
      $("desk-status").textContent = "已停止";
    }
  },

  open(agentId) {
    this.active = agentId;
    if (!this.canvas) {
      this.canvas = $("desk-canvas");
      this.ctx = this.canvas.getContext("2d");
      this.bindInput();
    }
    if (!this.streams[agentId]) this.start(agentId);
  },

  onMessage(msg) {
    if (msg.type !== "task_result" || !msg.action) return false;
    if (msg.action === "remote_start") {
      if (msg.ok) {
        try {
          const dim = JSON.parse(msg.data);
          this.streams[msg.agentId] = { taskId: msg.taskId, w: dim.w, h: dim.h };
          if (this.canvas) {
            this.canvas.width = dim.w;
            this.canvas.height = dim.h;
          }
          if (msg.agentId === this.active) {
            $("desk-status").textContent =
              `在线 ${dim.w}x${dim.h}`;
          }
        } catch (e) {}
        return true;
      }
      if (msg.agentId === this.active) {
        $("desk-status").textContent = "启动失败: " + (msg.error || "未知错误");
      }
      return true;
    }
    if (msg.action === "remote_stop") {
      return true;
    }
    return false;
  },

  _rxCount: 0,
  onBinary(frame) {
    this._rxCount = (this._rxCount || 0) + 1;
    if (frame.byteLength < 22) return false;
    const u8 = new Uint8Array(frame);
    if (u8[20] === 0x05) {
      const dv = new DataView(frame);
      const hotX = dv.getUint16(21);
      const hotY = dv.getUint16(23);
      const cw = dv.getUint16(25);
      const ch = dv.getUint16(27);
      const url = URL.createObjectURL(new Blob([u8.slice(29)], { type: "image/png" }));
      this.cursorShape = { hotX, hotY, w: cw, h: ch, url };
      const el = document.getElementById("desk-cursor");
      if (el) {
        el.style.backgroundImage = "url(" + url + ")";
      }
      return true;
    }
    if (u8[20] === 0x04) {
      const dv = new DataView(frame);
      const nx = dv.getFloat32(21, true);
      const ny = dv.getFloat32(25, true);
      const el = document.getElementById("desk-cursor");
      const stage = document.getElementById("desk-stage");
      const cv = this.canvas;
      if (el && stage && cv) {
        const sr = stage.getBoundingClientRect();
        const cr = cv.getBoundingClientRect();
        const k = cr.width / cv.width;
        const sh = this.cursorShape || { hotX: 0, hotY: 0, w: 16, h: 24 };
        const dispW = Math.max(16, sh.w * k * 2);
        const s = dispW / sh.w;
        el.style.width = dispW + "px";
        el.style.height = sh.h * s + "px";
        el.style.left = cr.left - sr.left + nx * cr.width - sh.hotX * s + "px";
        el.style.top = cr.top - sr.top + ny * cr.height - sh.hotY * s + "px";
        el.style.display = "block";
      }
      return true;
    }
    if (u8[20] === 0x07) {

      if (!this.canvas || !this.active) return true;
      const dv = new DataView(frame);
      const count = dv.getUint16(21);
      const snap = this._snapshot();
      snap.getContext("2d").drawImage(this.canvas, 0, 0);
      for (let i = 0; i < count; ++i) {
        const o = 23 + i * 12;
        const sx = dv.getUint16(o), sy = dv.getUint16(o + 2);
        const dx = dv.getUint16(o + 4), dy = dv.getUint16(o + 6);
        const w = dv.getUint16(o + 8), h = dv.getUint16(o + 10);
        if (w > 0 && h > 0)
          this.ctx.drawImage(snap, sx, sy, w, h, dx, dy, w, h);
      }
      return true;
    }
    if (u8[20] === 0x06) {
      if (!this.canvas || !this.active) return true;
      const dv = new DataView(frame);
      const cols = dv.getUint16(21), rows = dv.getUint16(23);
      const tw = dv.getUint16(25), th = dv.getUint16(27);
      const count = dv.getUint16(29);
      const dataStart = 31 + count * 8;
      let meta = 31, data = dataStart;
      for (let i = 0; i < count; ++i) {
        const tx = dv.getUint16(meta), ty = dv.getUint16(meta + 2);
        const len = dv.getUint32(meta + 4);
        meta += 8;
        if (data + len > frame.byteLength) break;
        const blob = new Blob([new Uint8Array(frame, data, len)], { type: "image/jpeg" });
        data += len;
        const dx = tx * tw, dy = ty * th;
        createImageBitmap(blob).then(bmp => {
          this.ctx.drawImage(bmp, dx, dy, bmp.width, bmp.height);
          bmp.close();
        }).catch(() => {});
      }
      return true;
    }
    if (u8[20] !== 0x03) return false;
    if (!this.canvas || !this.active) return false;
    const blob = new Blob([u8.slice(21)], { type: "image/jpeg" });
    createImageBitmap(blob).then(bmp => {
      this.ctx.drawImage(bmp, 0, 0, this.canvas.width, this.canvas.height);
      bmp.close();
    }).catch(() => {});
    return true;
  },

  _snapshot() {
    if (!this._snap || this._snap.width !== this.canvas.width ||
        this._snap.height !== this.canvas.height) {
      this._snap = document.createElement("canvas");
      this._snap.width = this.canvas.width;
      this._snap.height = this.canvas.height;
    }
    return this._snap;
  },

  bindInput() {
    const cv = this.canvas;
    let pending = null;
    cv.addEventListener("mousemove", e => {
      const now = Date.now();
      if (pending && now - pending.t < 33) return;
      pending = { t: now };
      const rect = cv.getBoundingClientRect();
      this.sendRaw({ kind: "move",
                     nx: (e.clientX - rect.left) / rect.width,
                     ny: (e.clientY - rect.top) / rect.height });
    });
    cv.addEventListener("mousedown", e => {
      this.sendRaw({ kind: "button", button: e.button, down: true });
      e.preventDefault();
    });
    cv.addEventListener("mouseup", e => {
      this.sendRaw({ kind: "button", button: e.button, down: false });
      e.preventDefault();
    });
    cv.addEventListener("contextmenu", e => e.preventDefault());
    cv.addEventListener("wheel", e => {
      this.sendRaw({ kind: "wheel", dy: e.deltaY < 0 ? 3 : -3 });
      e.preventDefault();
    }, { passive: false });

    window.addEventListener("keydown", e => {
      if (!this.isVisible()) return;
      if (e.code === "F5" || (e.ctrlKey && e.key === "r")) return;
      this.sendRaw({ kind: "key", code: e.code, down: true });
      e.preventDefault();
    }, true);
    window.addEventListener("keyup", e => {
      if (!this.isVisible()) return;
      this.sendRaw({ kind: "key", code: e.code, down: false });
      e.preventDefault();
    }, true);
  },

  sendRaw(ev) {
    const agentId = this.active;
    if (!agentId || !this.streams[agentId]) return;
    Ws.sendJson({ type: "task_req", agentId: agentId, action: "remote_input",
                  payload: { events: [ev] } });
  },

  isVisible() {
    const v = $("desktop-view");
    return v && !v.classList.contains("hidden");
  }
};
