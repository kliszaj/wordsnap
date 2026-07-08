const queueList = document.querySelector("#clip-queue-list");
const detail = document.querySelector("#clip-detail");
const queueTemplate = document.querySelector("#queue-template");
const detailTemplate = document.querySelector("#detail-template");
const fileInput = document.querySelector("#file-input");
const refreshButton = document.querySelector("#refresh-button");

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
    node.classList.toggle("active", clip.id === selectedClipId);
    node.querySelector(".queue-number").textContent = clipNumber(clip, index);
    node.querySelector(".queue-title").textContent = clip.corrected_word || clip.original_name;
    node.querySelector(".queue-meta").textContent = clip.iso_week || "unsorted";
    const status = node.querySelector(".status");
    status.className = statusClass(clip.status);
    status.textContent = clip.status;
    node.addEventListener("click", () => {
      selectedClipId = clip.id;
      render();
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
  node.querySelector(".clip-title").textContent = clip.corrected_word || clip.original_name;
  node.querySelector(".clip-meta").textContent = `${clip.original_name} · ${clip.iso_week || "unsorted"}`;
  const status = node.querySelector(".status");
  status.className = statusClass(clip.status);
  status.textContent = clip.status;
  node.querySelector(".audio").src = `/api/clips/${clip.id}/audio`;

  const transcript = node.querySelector(".transcript");
  const correctedWord = node.querySelector(".corrected-word");
  const swedish = node.querySelector(".swedish-definition");
  const english = node.querySelector(".english-definition");

  transcript.value = clip.transcript || "";
  correctedWord.value = clip.corrected_word || "";
  swedish.value = clip.swedish_definition || "";
  english.value = clip.english_definition || "";

  node.querySelector(".front").textContent = clip.anki?.front || clip.corrected_word || "Front";
  node.querySelector(".back").textContent = ankiBack(clip);

  node.querySelector(".process").addEventListener("click", async () => {
    await request(`/api/clips/${clip.id}/process`, {
      method: "POST",
      body: JSON.stringify({
        transcript: transcript.value,
        corrected_word: correctedWord.value,
        iso_week: clip.iso_week,
      }),
    });
    await loadClips();
  });

  node.querySelector(".save").addEventListener("click", async () => {
    await request(`/api/clips/${clip.id}`, {
      method: "PATCH",
      body: JSON.stringify({
        transcript: transcript.value,
        corrected_word: correctedWord.value,
        swedish_definition: swedish.value,
        english_definition: english.value,
      }),
    });
    await loadClips();
  });

  node.querySelector(".approve").addEventListener("click", async () => {
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

refreshButton.addEventListener("click", loadClips);
loadClips().catch((error) => {
  queueList.innerHTML = `<div class="empty queue-empty">Could not load clips: ${error.message}</div>`;
  detail.innerHTML = `<div class="empty">Could not load clip detail.</div>`;
});
