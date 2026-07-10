const queueList = document.querySelector("#clip-queue-list");
const detail = document.querySelector("#clip-detail");
const queueTemplate = document.querySelector("#queue-template");
const detailTemplate = document.querySelector("#detail-template");
const fileInput = document.querySelector("#file-input");

let clips = [];
let selectedClipId = null;
const queueAudio = new Audio();
let playingClipId = null;

function statusClass(status) {
  if (status === "approved") return "status approved";
  if (status === "error") return "status error";
  if (status === "exported") return "status exported";
  if (["processed", "needs_review"].includes(status)) return "status ready";
  if (status === "processing") return "status processing";
  return "status";
}

function statusLabel(status) {
  const labels = {
    uploaded: "uploaded",
    processing: "processing",
    processed: "ready",
    needs_review: "ready",
    approved: "csv ready",
    exported: "saved",
    error: "error",
  };
  return labels[status] || status || "unknown";
}

function isTypingTarget(target) {
  if (!target) return false;
  if (target.closest?.(".queue-item")) return false;
  return ["INPUT", "TEXTAREA", "SELECT", "BUTTON", "A", "AUDIO"].includes(target.tagName) || target.isContentEditable;
}

function clipNumber(clip, index) {
  const match = clip.original_name?.match(/(\d{3,})/);
  if (match) return match[1].slice(-3);
  return String(index + 1).padStart(3, "0");
}

function clipFront(clip) {
  return clip.card_front || clip.corrected_word || clip.original_name;
}

function ankiBack(clip) {
  if (clip.anki?.back) return clip.anki.back.replaceAll("<br>", "\n");
  if (clip.swedish_definition || clip.english_definition) {
    return `SWE: ${clip.swedish_definition || ""}\nENG: ${clip.english_definition || ""}`;
  }
  return "SWE:\nENG:";
}

function setMetrics() {
  const total = document.querySelector("#metric-total");
  const ready = document.querySelector("#metric-ready");
  const saved = document.querySelector("#metric-saved");
  const pending = document.querySelector("#pending-count");
  if (!total || !ready || !saved || !pending) return;

  total.textContent = String(clips.length).padStart(2, "0");
  ready.textContent = String(
    clips.filter((clip) => ["processed", "needs_review"].includes(clip.status)).length,
  ).padStart(2, "0");
  saved.textContent = String(
    clips.filter((clip) => ["approved", "exported"].includes(clip.status)).length,
  ).padStart(2, "0");
  pending.textContent = `[${String(
    clips.filter((clip) => clip.status !== "approved" && clip.status !== "exported").length,
  ).padStart(2, "0")}]`;
}

function hasProcessingClips() {
  return clips.some((clip) => ["uploaded", "processing"].includes(clip.status));
}

async function request(path, options = {}) {
  const response = await fetch(path, {
    headers: options.body instanceof FormData ? {} : { "Content-Type": "application/json" },
    ...options,
  });
  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || response.statusText);
  }
  if (response.status === 204) return {};
  return response.json();
}

async function loadClips() {
  clips = await request("/api/clips");
  if (!clips.some((clip) => clip.id === selectedClipId)) {
    selectedClipId = clips[0]?.id || null;
  }
  render();
}

function selectClipAt(index) {
  if (!clips.length) return;
  const nextIndex = Math.max(0, Math.min(clips.length - 1, index));
  selectedClipId = clips[nextIndex].id;
  render();
  document.querySelector(".queue-item.active")?.scrollIntoView({ block: "nearest" });
}

function selectedClipIndex() {
  return clips.findIndex((clip) => clip.id === selectedClipId);
}

function setQueuePlaybackState() {
  document.querySelectorAll(".queue-row").forEach((row) => {
    const isPlayingRow = row.dataset.clipId === playingClipId && !queueAudio.paused;
    const progress = row.dataset.clipId === playingClipId && queueAudio.duration
      ? Math.min(1, queueAudio.currentTime / queueAudio.duration)
      : 0;
    const play = row.querySelector(".queue-play");
    play?.classList.toggle("playing", isPlayingRow);
    play?.style.setProperty("--progress", `${progress * 360}deg`);
    play?.setAttribute("aria-label", isPlayingRow ? "Pause snip" : "Play snip");
  });
}

async function toggleQueuePlayback(clip) {
  if (playingClipId === clip.id && !queueAudio.paused) {
    queueAudio.pause();
    setQueuePlaybackState();
    return;
  }
  document.querySelectorAll(".audio").forEach((audio) => audio.pause());
  if (playingClipId !== clip.id) {
    playingClipId = clip.id;
    queueAudio.src = `/api/clips/${clip.id}/audio`;
    queueAudio.currentTime = 0;
  }
  await queueAudio.play();
  setQueuePlaybackState();
}

async function toggleSelectedClipSaved() {
  const clip = clips.find((item) => item.id === selectedClipId);
  if (!clip || clip.status === "exported") return;
  await request(`/api/clips/${clip.id}/save-to-anki`, { method: "POST", body: JSON.stringify({}) });
  await loadClips();
}

function renderQueue() {
  queueList.textContent = "";
  if (!clips.length) {
    const empty = document.createElement("div");
    empty.className = "empty queue-empty";
    empty.textContent = "No snips yet. Upload WAV files or sync from the device.";
    queueList.append(empty);
    return;
  }

  clips.forEach((clip, index) => {
    const node = queueTemplate.content.firstElementChild.cloneNode(true);
    node.dataset.clipId = clip.id;
    const item = node.querySelector(".queue-item");
    node.classList.toggle("active", clip.id === selectedClipId);
    item.classList.toggle("active", clip.id === selectedClipId);
    node.querySelector(".queue-title").textContent = clipFront(clip);
    node.querySelector(".queue-meta").textContent = `SNIP ${clipNumber(clip, index)}`;
    const playButton = node.querySelector(".queue-play");
    playButton.addEventListener("click", async (event) => {
      event.stopPropagation();
      selectedClipId = clip.id;
      render();
      try {
        await toggleQueuePlayback(clip);
      } catch (error) {
        console.error("Could not play snip", error);
      }
    });
    item.addEventListener("click", () => {
      selectedClipId = clip.id;
      render();
    });
    node.querySelector(".queue-delete").addEventListener("click", async (event) => {
      event.stopPropagation();
      if (playingClipId === clip.id) {
        queueAudio.pause();
        playingClipId = null;
      }
      await request(`/api/clips/${clip.id}`, { method: "DELETE" });
      if (selectedClipId === clip.id) selectedClipId = null;
      await loadClips();
    });
    queueList.append(node);
  });
  setQueuePlaybackState();
}

function renderDetail() {
  detail.textContent = "";
  const clip = clips.find((item) => item.id === selectedClipId);
  if (!clip) {
    const empty = document.createElement("div");
    empty.className = "empty";
    empty.textContent = clips.length ? "Select a snip from the ledger." : "The selected snip will appear here.";
    detail.append(empty);
    return;
  }

  const node = detailTemplate.content.firstElementChild.cloneNode(true);
  node.querySelector(".clip-title").textContent = clipFront(clip);
  node.querySelector(".clip-meta").textContent = `${clip.original_name} · ${clip.iso_week || "unsorted"}`;
  const status = node.querySelector(".status");
  status.className = statusClass(clip.status);
  status.textContent = statusLabel(clip.status);
  const detailAudio = node.querySelector(".audio");
  detailAudio.src = `/api/clips/${clip.id}/audio`;
  detailAudio.addEventListener("play", () => queueAudio.pause());

  const transcript = node.querySelector(".transcript");
  const cardFront = node.querySelector(".card-front") || node.querySelector(".corrected-word");

  transcript.value = clip.transcript || "";
  cardFront.value = clip.card_front || clip.corrected_word || "";

  node.querySelector(".front").textContent = clip.anki?.front || clip.card_front || clip.corrected_word || "Front";
  node.querySelector(".back").textContent = ankiBack(clip);

  const syncButton = node.querySelector(".pipeline-sync");
  if (clip.status === "exported") {
    syncButton.textContent = "Synced";
    syncButton.disabled = true;
  }

  syncButton.addEventListener("click", async () => {
    syncButton.disabled = true;
    syncButton.textContent = "Syncing";
    const processed = await request(`/api/clips/${clip.id}/process`, {
      method: "POST",
      body: JSON.stringify({
        transcript: transcript.value,
        card_front: cardFront.value,
        iso_week: clip.iso_week,
      }),
    });
    await request(`/api/clips/${processed.id || clip.id}/save-to-anki`, {
      method: "POST",
      body: JSON.stringify({}),
    });
    await loadClips();
  });

  node.querySelector(".error").textContent = clip.error || clip.anki_sync_error || "";
  detail.append(node);
}

function render() {
  setMetrics();
  renderQueue();
  renderDetail();
}

queueAudio.addEventListener("timeupdate", setQueuePlaybackState);
queueAudio.addEventListener("play", setQueuePlaybackState);
queueAudio.addEventListener("pause", setQueuePlaybackState);
queueAudio.addEventListener("ended", () => {
  queueAudio.currentTime = 0;
  setQueuePlaybackState();
});

fileInput?.addEventListener("change", async () => {
  for (const file of fileInput.files) {
    const form = new FormData();
    form.append("file", file);
    const uploaded = await request("/api/upload", { method: "POST", body: form });
    selectedClipId = uploaded.id || selectedClipId;
  }
  fileInput.value = "";
  await loadClips();
});

loadClips().catch((error) => {
  queueList.innerHTML = `<div class="empty queue-empty">Could not load snips: ${error.message}</div>`;
  detail.innerHTML = `<div class="empty">Could not load snip detail.</div>`;
});

// ---- Settings (LLM credentials, persisted to appdata) ----
const settingsDialog = document.querySelector("#settings-dialog");
const settingsOpen = document.querySelector("#settings-open");
const settingsClose = document.querySelector("#settings-close");
const settingsSave = document.querySelector("#settings-save");
const settingsMsg = document.querySelector("#settings-msg");
const inputAnthropic = document.querySelector("#input-anthropic");
const inputOpenai = document.querySelector("#input-openai");
const inputLlmProvider = document.querySelector("#input-llm-provider");
const inputClaudeModel = document.querySelector("#input-claude-model");
const inputWhisperModel = document.querySelector("#input-whisper-model");
const inputGptModel = document.querySelector("#input-gpt-model");
const inputAnkiConnectUrl = document.querySelector("#input-anki-connect-url");
const inputAnkiDeck = document.querySelector("#input-anki-deck");
const statusAnthropic = document.querySelector("#status-anthropic");
const statusOpenai = document.querySelector("#status-openai");
const statusAnkiConnect = document.querySelector("#status-anki-connect");
const statusAnkiDeck = document.querySelector("#status-anki-deck");
const statusAnkiLastSave = document.querySelector("#status-anki-last-save");

function renderSecretStatus(el, secret) {
  el.textContent = secret.configured ? `set · ${secret.hint}` : "not set";
  el.className = `settings-status ${secret.configured ? "ok" : "missing"}`;
}

function renderSettings(data) {
  renderSecretStatus(statusAnthropic, data.secrets.anthropic);
  renderSecretStatus(statusOpenai, data.secrets.openai);
  inputLlmProvider.value = data.llm_provider || "anthropic";
  inputClaudeModel.placeholder = data.models.claude.value;
  inputWhisperModel.placeholder = data.models.whisper.value;
  inputGptModel.placeholder = data.models.gpt.value;
  inputAnkiConnectUrl.placeholder = data.anki.connect_url || "http://10.0.0.x:8765";
  inputAnkiDeck.placeholder = data.anki.deck || "Default";
  const connection = data.anki.connection || {};
  statusAnkiConnect.textContent = connection.connected ? `connected · v${connection.version}` : connection.error || "not connected";
  statusAnkiConnect.className = `settings-status ${connection.connected ? "ok" : "missing"}`;
  statusAnkiDeck.textContent = connection.deck_exists ? "found" : "will create on save";
  statusAnkiDeck.className = `settings-status ${connection.deck_exists ? "ok" : "missing"}`;
  statusAnkiLastSave.textContent = connection.last_saved_at || "none yet";
}

function clearSettingsInputs() {
  for (const el of [
    inputAnthropic,
    inputOpenai,
    inputClaudeModel,
    inputWhisperModel,
    inputGptModel,
    inputAnkiConnectUrl,
    inputAnkiDeck,
  ]) {
    el.value = "";
  }
}

async function openSettings() {
  settingsMsg.textContent = "";
  clearSettingsInputs();
  try {
    renderSettings(await request("/api/settings"));
  } catch (error) {
    settingsMsg.textContent = `Could not load settings: ${error.message}`;
  }
  if (typeof settingsDialog.showModal === "function") settingsDialog.showModal();
  else settingsDialog.setAttribute("open", "");
}

async function saveSettings() {
  const payload = {};
  const fields = [
    [inputAnthropic, "anthropic_api_key"],
    [inputOpenai, "openai_api_key"],
    [inputLlmProvider, "llm_provider"],
    [inputClaudeModel, "claude_model"],
    [inputWhisperModel, "whisper_model"],
    [inputGptModel, "gpt_model"],
    [inputAnkiConnectUrl, "anki_connect_url"],
    [inputAnkiDeck, "anki_deck"],
  ];
  for (const [el, key] of fields) {
    const value = el.value.trim();
    if (value) payload[key] = value;
  }
  if (Object.keys(payload).length === 0) {
    settingsMsg.textContent = "Nothing to save — fill in a field first.";
    return;
  }
  settingsMsg.textContent = "Saving…";
  try {
    const data = await request("/api/settings", { method: "POST", body: JSON.stringify(payload) });
    renderSettings(data);
    clearSettingsInputs();
    settingsMsg.textContent = "Saved.";
  } catch (error) {
    settingsMsg.textContent = `Save failed: ${error.message}`;
  }
}

settingsOpen?.addEventListener("click", openSettings);
settingsClose?.addEventListener("click", () => settingsDialog.close());
settingsSave?.addEventListener("click", saveSettings);

document.addEventListener("keydown", async (event) => {
  if (isTypingTarget(event.target) || settingsDialog.open) return;
  if (event.key === "ArrowDown") {
    event.preventDefault();
    selectClipAt(selectedClipIndex() + 1);
  } else if (event.key === "ArrowUp") {
    event.preventDefault();
    selectClipAt(selectedClipIndex() - 1);
  } else if (event.code === "Space") {
    event.preventDefault();
    await toggleSelectedClipSaved();
  }
});

setInterval(() => {
  if (!hasProcessingClips() || isTypingTarget(document.activeElement) || settingsDialog.open) return;
  loadClips().catch(() => {});
}, 5000);

// ---- Auto-sync toggle (auto-save new snips to Anki + sync to phone) ----
const autoSyncInput = document.querySelector("#auto-sync-input");

async function loadAutoSync() {
  try {
    const data = await request("/api/auto-sync");
    autoSyncInput.checked = Boolean(data.enabled);
  } catch {
    // leave the default (checked) if the server can't be reached
  }
}

autoSyncInput?.addEventListener("change", async () => {
  const enabled = autoSyncInput.checked;
  try {
    const data = await request("/api/auto-sync", {
      method: "POST",
      body: JSON.stringify({ enabled }),
    });
    autoSyncInput.checked = Boolean(data.enabled);
  } catch {
    autoSyncInput.checked = !enabled; // revert on failure
  }
});

loadAutoSync();
