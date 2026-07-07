const list = document.querySelector("#clip-list");
const template = document.querySelector("#clip-template");
const fileInput = document.querySelector("#file-input");
const refreshButton = document.querySelector("#refresh-button");

let clips = [];

function statusClass(status) {
  if (status === "approved") return "status approved";
  if (status === "error") return "status error";
  return "status";
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
  render();
}

function render() {
  setMetrics();
  list.textContent = "";
  if (!clips.length) {
    const empty = document.createElement("div");
    empty.className = "empty";
    empty.textContent = "No clips yet. Upload WAV files or sync from the device.";
    list.append(empty);
    return;
  }

  for (const clip of clips) {
    const node = template.content.firstElementChild.cloneNode(true);
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
    list.append(node);
  }
}

fileInput.addEventListener("change", async () => {
  for (const file of fileInput.files) {
    const form = new FormData();
    form.append("file", file);
    await request("/api/upload", { method: "POST", body: form });
  }
  fileInput.value = "";
  await loadClips();
});

refreshButton.addEventListener("click", loadClips);
loadClips().catch((error) => {
  list.innerHTML = `<div class="empty">Could not load clips: ${error.message}</div>`;
});
