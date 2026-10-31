// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: optional IndexedDB persistence for the working library. Every call is wrapped so
// the page keeps working (in memory) when storage is blocked, full or unavailable.

const DB = "fm4-webapp", STORE = "kv", VERSION = 1;
let opening = null;

function open() {
  if (opening) return opening;
  opening = new Promise((resolve) => {
    try {
      if (!globalThis.indexedDB) return resolve(null);
      const req = indexedDB.open(DB, VERSION);
      req.onupgradeneeded = () => { try { req.result.createObjectStore(STORE); } catch { /* exists */ } };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => resolve(null);
      req.onblocked = () => resolve(null);
    } catch { resolve(null); }
  });
  return opening;
}

export async function load(key) {
  try {
    const db = await open(); if (!db) return undefined;
    return await new Promise((resolve) => {
      const r = db.transaction(STORE, "readonly").objectStore(STORE).get(key);
      r.onsuccess = () => resolve(r.result); r.onerror = () => resolve(undefined);
    });
  } catch { return undefined; }
}

export async function save(key, value) {
  try {
    const db = await open(); if (!db) return false;
    return await new Promise((resolve) => {
      const tx = db.transaction(STORE, "readwrite");
      tx.objectStore(STORE).put(value, key);
      tx.oncomplete = () => resolve(true); tx.onerror = () => resolve(false); tx.onabort = () => resolve(false);
    });
  } catch { return false; }
}
