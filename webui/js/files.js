const Files = {
  CHUNK: 262144,
  WINDOW: 4,
  _dl: null,
  _up: null,
  download(agentId, path) {
    if (!this._dl || this._dl.path !== path || this._dl.done) {
      this._dl = {
        path, agentId, taskId: "", chunks: [], received: 0, size: 0, sha: "", name: path.split(/[\\/]/).pop(), done: false
      }
    }
    this._dl.agentId = agentId;
    const resumeFrom = this._dl.received;
    Ws.sendJson({
      type: "task_req",
      agentId: agentId,
      action: "file_download",
      payload: {
        path: path,
        resumeFrom: resumeFrom,
        chunkSize: this.CHUNK,
        windowSize: this.WINDOW
      }
    });
    $("files-status").textContent = (resumeFrom > 0 ? "续传中: " : "下载中: ") + path + (resumeFrom > 0 ? `(从${resumeFrom}B)` : "")
  },
  async upload(agentId, file, remotePath) {
    const buf = await file.arrayBuffer();
    const hashBuf = await crypto.subtle.digest("SHA-256", buf);
    const sha = [...new Uint8Array(hashBuf)].map(b => b.toString(16).padStart(2, "0")).join("");
    this._up = {
      buf, sha, size: buf.byteLength, name: file.name, path: remotePath, agentId, taskId: "", nextOffset: 0, ackedOffset: 0, seq: 0, done: false
    };
    Ws.sendJson({
      type: "task_req",
      agentId: agentId,
      action: "file_upload",
      payload: {
        path: remotePath,
        size: buf.byteLength,
        sha256: sha,
        filename: file.name,
        resumeFrom: 0
      }
    });
    $("files-status").textContent = "上传准备中: " + remotePath
  },
  _pumpUpload() {
    const up = this._up;
    if (!up || up.done || !up.taskId) return;
    const hex = up.taskId.replace(/-/g, "");
    while (up.nextOffset < up.size && (up.nextOffset - up.ackedOffset) < this.WINDOW * this.CHUNK) {
      const take = Math.min(this.CHUNK, up.size - up.nextOffset);
      const frame = new Uint8Array(29 + take);
      for (let i = 0; i < 16; ++i) frame[i] = parseInt(hex.substr(i * 2, 2), 16);
      frame[16] = (up.seq >>> 24) & 0xFF;
      frame[17] = (up.seq >>> 16) & 0xFF;
      frame[18] = (up.seq >>> 8) & 0xFF;
      frame[19] = up.seq & 0xFF;
      frame[20] = 0x01;
      let off = up.nextOffset;
      for (let i = 0; i < 8; ++i) {
        frame[28 - i] = off % 256;
        off = Math.floor(off / 256)
      }
      frame.set(new Uint8Array(up.buf, up.nextOffset, take), 29);
      Ws.sendBinary(frame);
      up.nextOffset += take;
      up.seq++
    }
  },
  onMessage(msg) {
    if (msg.type === "file_ack") {
      if (this._up && msg.taskId === this._up.taskId) {
        if (msg.ackedOffset > this._up.ackedOffset) this._up.ackedOffset = msg.ackedOffset;
        this._pumpUpload();
        return true
      }
      return false
    }
    if (msg.type !== "task_result") return false;
    if (msg.action === "file_download" && msg.data && msg.data.indexOf("\"sha256\"") !== -1 && this._dl) {
      try {
        const summary = JSON.parse(msg.data);
        this._dl.size = summary.size;
        this._dl.sha = summary.sha256;
        if (!this._dl.taskId) this._dl.taskId = msg.taskId;
        this._finishDownload();
        return true
      } catch (e) {
        return false
      }
    }
    if (msg.action === "file_upload" && this._up) {
      let ready = null;
      try {
        ready = JSON.parse(msg.data)
      } catch (e) {}
      if (ready && ready.ready) {
        this._up.taskId = msg.taskId;
        const rf = ready.resumeFrom || 0;
        this._up.nextOffset = rf;
        this._up.ackedOffset = rf;
        this._up.seq = 0;
        $("files-status").textContent = rf > 0 ? `续传上传中...(从${rf}B)` : "上传中...";
        this._pumpUpload();
        return true
      }
      if (msg.ok && msg.data === "ok") {
        this._up.done = true;
        this._up = null;
        $("files-status").textContent = "上传完成";
        return true
      }
      $("files-status").textContent = "上传失败: " + (msg.error || msg.data || "未知错误");
      this._up = null;
      return true
    }
    return false
  },
  onBinary(frame) {
    if (frame.byteLength < 29) return false;
    const u8 = new Uint8Array(frame);
    if (u8[20] !== 0x01) return false;
    const dl = this._dl;
    if (!dl) return true;
    if (!dl.taskId) {
      const hex = [...u8.slice(0, 16)].map(b => b.toString(16).padStart(2, "0")).join("");
      dl.taskId = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12)
    }
    const dv = new DataView(frame);
    const offset = Number(dv.getBigUint64(21, false));
    const data = u8.slice(29);
    if (offset === dl.received) {
      dl.chunks.push(data);
      dl.received = offset + data.length;
      Ws.sendJson({
        type: "file_ack",
        taskId: dl.taskId,
        ackedOffset: dl.received
      })
    }
    return true
  },
  _finishDownload() {
    const dl = this._dl;
    let total = 0;
    for (const c of dl.chunks) total += c.length;
    const merged = new Uint8Array(total);
    let off = 0;
    for (const c of dl.chunks) {
      merged.set(c, off);
      off += c.length
    }
    crypto.subtle.digest("SHA-256", merged).then(hashBuf => {
      const sha = [...new Uint8Array(hashBuf)].map(b => b.toString(16).padStart(2, "0")).join("");
      if (sha !== dl.sha) {
        $("files-status").textContent = `下载校验失败(sha256不符，收到${total}/${dl.size}B)`;
        return
      }
      const blob = new Blob([merged], {
        type: "application/octet-stream"
      });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = dl.name;
      a.click();
      $("files-status").textContent = `下载完成:${dl.name}(${total}B)`;
      dl.done = true
    })
  }
};