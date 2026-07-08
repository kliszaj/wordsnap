const queueList = document.querySelector("#clip-queue-list");
const detail = document.querySelector("#clip-detail");
const queueTemplate = document.querySelector("#queue-template");
const detailTemplate = document.querySelector("#detail-template");
const fileInput = document.querySelector("#file-input");

let clips = [];
let selectedClipId = null;

function statusClass(status) {
  if (status === "approved") return "status approved";
  if (status === "error") return "status error";
  if (status === "exported") return "status exported";
  return "status";
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
  document.querySelector("#metric-total").textContent = String(clips.length).padStart(2, "0");
  document.querySelector("#metric-review").textContent = String(
    clips.filter((clip) => ["processed", "needs_review"].includes(clip.status)).length,
  ).padStart(2, "0");
  document.querySelector("#metric-approved").textContent = String(
    clips.filter((clip) => clip.status === "approved").length,
  ).padStart(2, "0");
  document.querySelector("#pending-count").textContent = `[${String(
    clips.filter((clip) => clip.status !== "approved" && clip.status !== "exported").length,
  ).padStart(2, "0")}]`;
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

function renderQueue() {
  queueList.textContent = "";
  if (!clips.length) {
    const empty = document.createElement("div");
    empty.className = "empty queue-empty";
    empty.textContent = "No clips yet. Upload WAV files or sync from the device.";
    queueList.append(empty);
    return;
  }

  clips.forEach((clip, index) => {
    const node = queueTemplate.content.firstElementChild.cloneNode(true);
    const item = node.querySelector(".queue-item");
    item.classList.toggle("active", clip.id === selectedClipId);
    node.querySelector(".queue-number").textContent = clipNumber(clip, index);
    node.querySelector(".queue-title").textContent = clipFront(clip);
    node.querySelector(".queue-meta").textContent = clip.iso_week || "unsorted";
    const status = node.querySelector(".status");
    status.className = statusClass(clip.status);
    status.textContent = clip.status;
    item.addEventListener("click", () => {
      selectedClipId = clip.id;
      render();
    });
    node.querySelector(".queue-delete").addEventListener("click", async () => {
      if (!confirm(`Delete ${clip.original_name}?`)) return;
      await request(`/api/clips/${clip.id}`, { method: "DELETE" });
      if (selectedClipId === clip.id) selectedClipId = null;
      await loadClips();
    });
    queueList.append(node);
  });
}

function renderDetail() {
  detail.textContent = "";
  const clip = clips.find((item) => item.id === selectedClipId);
  if (!clip) {
    const empty = document.createElement("div");
    empty.className = "empty";
    empty.textContent = clips.length ? "Select a clip from the ledger." : "The selected clip will appear here.";
    detail.append(empty);
    return;
  }

  const node = detailTemplate.content.firstElementChild.cloneNode(true);
  node.querySelector(".clip-title").textContent = clipFront(clip);
  node.querySelector(".clip-meta").textContent = `${clip.original_name} · ${clip.iso_week || "unsorted"}`;
  const status = node.querySelector(".status");
  status.className = statusClass(clip.status);
  status.textContent = clip.status;
  node.querySelector(".audio").src = `/api/clips/${clip.id}/audio`;

  const transcript = node.querySelector(".transcript");
  const cardFront = node.querySelector(".card-front");
  const swedish = node.querySelector(".swedish-definition");
  const english = node.querySelector(".english-definition");

  transcript.value = clip.transcript || "";
  cardFront.value = clip.card_front || clip.corrected_word || "";
  swedish.value = clip.swedish_definition || "";
  english.value = clip.english_definition || "";

  node.querySelector(".front").textContent = clip.anki?.front || clip.card_front || clip.corrected_word || "Front";
  node.querySelector(".back").textContent = ankiBack(clip);

  node.querySelector(".process").addEventListener("click", async () => {
    await request(`/api/clips/${clip.id}/process`, {
      method: "POST",
      body: JSON.stringify({
        transcript: transcript.value,
        card_front: cardFront.value,
        iso_week: clip.iso_week,
      }),
    });
    await loadClips();
  });

  node.querySelector(".save-anki").addEventListener("click", async () => {
    await request(`/api/clips/${clip.id}`, {
      method: "PATCH",
      body: JSON.stringify({
        transcript: transcript.value,
        card_front: cardFront.value,
        swedish_definition: swedish.value,
        english_definition: english.value,
      }),
    });
    await request(`/api/clips/${clip.id}/approve`, { method: "POST", body: JSON.stringify({}) });
    await loadClips();
  });

  node.querySelector(".error").textContent = clip.error || "";
  detail.append(node);
}

function render() {
  setMetrics();
  renderQueue();
  renderDetail();
}

fileInput.addEventListener("change", async () => {
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
  queueList.innerHTML = `<div class="empty queue-empty">Could not load clips: ${error.message}</div>`;
  detail.innerHTML = `<div class="empty">Could not load clip detail.</div>`;
});

// ---- Settings (LLM credentials, persisted to appdata) ----
const settingsDialog = document.querySelector("#settings-dialog");
const settingsOpen = document.querySelector("#settings-open");
const settingsClose = document.querySelector("#settings-close");
const settingsSave = document.querySelector("#settings-save");
const settingsMsg = document.querySelector("#settings-msg");
const inputAnthropic = document.querySelector("#input-anthropic");
const inputOpenai = document.querySelector("#input-openai");
const inputClaudeModel = document.querySelector("#input-claude-model");
const inputWhisperModel = document.querySelector("#input-whisper-model");
const inputGptModel = document.querySelector("#input-gpt-model");
const statusAnthropic = document.querySelector("#status-anthropic");
const statusOpenai = document.querySelector("#status-openai");

function renderSecretStatus(el, secret) {
  el.textContent = secret.configured ? `set · ${secret.hint}` : "not set";
  el.className = `settings-status ${secret.configured ? "ok" : "missing"}`;
}

function renderSettings(data) {
  renderSecretStatus(statusAnthropic, data.secrets.anthropic);
  renderSecretStatus(statusOpenai, data.secrets.openai);
  inputClaudeModel.placeholder = data.models.claude.value;
  inputWhisperModel.placeholder = data.models.whisper.value;
  inputGptModel.placeholder = data.models.gpt.value;
}

function clearSettingsInputs() {
  for (const el of [inputAnthropic, inputOpenai, inputClaudeModel, inputWhisperModel, inputGptModel]) {
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
    [inputClaudeModel, "claude_model"],
    [inputWhisperModel, "whisper_model"],
    [inputGptModel, "gpt_model"],
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
