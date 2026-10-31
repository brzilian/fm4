// SPDX-License-Identifier: GPL-3.0-only
// FM-4 SysEx extension commands (non-commercial ID 7D, "F4"). This is the single place where the
// webapp defines them; the firmware side must match these bytes exactly.
//
// Why they exist: the FM-1 SDK receives SysEx frames of at most 1024 bytes and sends messages of at
// most 128 bytes, so a 4104-byte TQ5 VMEM bulk can neither be sent to nor received from the device.
// Device bank transfers therefore move one voice at a time through the edit buffer:
//   write slot s: EFEDS, ACED2, ACED, VCED (the 4 TQ5 voice messages, each <= 101 bytes) to part p,
//                 then STORE(p, s)
//   read slot s:  READ(s) -> the device loads s into part 0's edit buffer and replies with the
//                 191-byte TQ5 voice dump (EFEDS, ACED2, ACED, VCED as 4 separate messages)
// .syx files keep the full TQ5 formats (VMEM, 100-voice banks); that is file-side only.

export const FM4EXT_ID = Object.freeze([0x7D, 0x46, 0x34]);   // 7D 'F' '4'

export const FM4EXT_CMD = Object.freeze({
  STORE: 0x01,    // F0 7D 46 34 01 part slot F7             store part's edit buffer to user slot 0-99
  READ: 0x02,     // F0 7D 46 34 02 slot F7                  load user slot into part 0, reply 191-byte dump
  SELECT: 0x03,   // F0 7D 46 34 03 part bank program F7     bank 0 preset, 1 user; program 0-99
});

export const FM4EXT_BANK = Object.freeze({ PRESET: 0, USER: 1 });
export const USER_SLOTS = 100;
export const DEVICE_MAX_RX = 1024;   // largest SysEx frame the device accepts
export const DEVICE_MAX_TX = 128;    // largest message the device sends

// Pacing used for one-voice-at-a-time bank transfers (milliseconds).
export const PACE = Object.freeze({ message: 20, afterStore: 150, readTimeout: 1500 });

const u7 = (v, what) => {
  if (!Number.isInteger(v) || v < 0 || v > 127) throw new RangeError(`${what} must be 0-127`);
  return v;
};
const slotOk = (s) => { if (!Number.isInteger(s) || s < 0 || s >= USER_SLOTS) throw new RangeError("slot must be 0-99"); return s; };
const partOk = (p) => { if (!Number.isInteger(p) || p < 0 || p > 7) throw new RangeError("part must be 0-7"); return p; };

export const storeMsg = (part, slot) =>
  Uint8Array.of(0xF0, ...FM4EXT_ID, FM4EXT_CMD.STORE, partOk(part), slotOk(slot), 0xF7);
export const readMsg = (slot) =>
  Uint8Array.of(0xF0, ...FM4EXT_ID, FM4EXT_CMD.READ, slotOk(slot), 0xF7);
export const selectMsg = (part, bank, program) => {
  if (bank !== 0 && bank !== 1) throw new RangeError("bank must be 0 (preset) or 1 (user)");
  if (!Number.isInteger(program) || program < 0 || program > 99) throw new RangeError("program must be 0-99");
  return Uint8Array.of(0xF0, ...FM4EXT_ID, FM4EXT_CMD.SELECT, partOk(part), bank, u7(program, "program"), 0xF7);
};

export function isFm4Ext(msg) {
  return msg.length >= 6 && msg[0] === 0xF0 && msg[1] === 0x7D && msg[2] === 0x46 && msg[3] === 0x34 && msg.at(-1) === 0xF7;
}
