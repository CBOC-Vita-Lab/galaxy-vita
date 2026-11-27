// Player name rules. The game stores 11 UTF-16 units including the terminator (NetworkRankingSubmission.mName),
// so a name has at most 10 characters. Only plain ASCII so every font and every console renders it.

import { ApiError } from './util.js';

export const NAME_MIN = 2;
export const NAME_MAX = 10;

// The filter (svcfix2): ONE list (name_filter.js) and ONE normaliser, shared with the game (platform/src/mp/mp_name_filter.h is generated from the
// same file by scripts/gen-name-filter.mjs; the game applies it to names that arrive from peers on a local Wi-Fi, where there is no service). A name
// is not matched as typed but as it SOUNDS: look-alike letters and digits are folded ("Sh1t", "Nigg3r", "Phuck", "Fvck", "Hitl3r"), separators
// are dropped ("F.u.c.k"), and repeats are collapsed ("Fuuuck", "Shiit"). It is still not a moderation system: it stops the obvious.
//   substring  (4+ letters)   blocked anywhere in the folded name
//   word                       blocked as a whole word of the name (words split at space . _ - and at a lower-to-upper case change, digits around a
//                              word ignored, optional plural s): "GrapeRape" and "Dick Smith" yes, "Grape", "Dickson" no
//   exact                      blocked only as the whole name
//   reservedExact              staff-like names, as the whole name (as before)
//   reservedWord / Prefix      impersonation: a word that is "admin", "staff", "cboc" ... or starts with a distinctive one ("Admin1", "CBOC_Staff",
//                              "Moderator7"), so nobody can look like the people who run the game; "Badminton" is fine
import list from './name_filter.js';

const LEET = { 0: 'o', 1: 'l', 3: 'e', 4: 'a', 5: 's', 7: 't', 8: 'b', 9: 'g', '@': 'a', $: 's', '!': 'l', '+': 't', '|': 'l' };
// a few look-alikes outside ASCII (the service only takes ASCII names, the game sees whatever a stranger's console sends)
const WIDE = {
  0xe0: 'a', 0xe1: 'a', 0xe2: 'a', 0xe3: 'a', 0xe4: 'a', 0xe5: 'a', 0xe7: 'c', 0xe8: 'e', 0xe9: 'e', 0xea: 'e', 0xeb: 'e', 0xec: 'i', 0xed: 'i', 0xee: 'i', 0xef: 'i',
  0xf1: 'n', 0xf2: 'o', 0xf3: 'o', 0xf4: 'o', 0xf5: 'o', 0xf6: 'o', 0xf9: 'u', 0xfa: 'u', 0xfb: 'u', 0xfc: 'u', 0xfd: 'y', 0xff: 'y',
  0x430: 'a', 0x435: 'e', 0x43e: 'o', 0x440: 'p', 0x441: 'c', 0x445: 'x', 0x443: 'y', 0x43a: 'k',
};

/** One character -> the letter it stands for, or '' (a separator or something unknown). Letters i / v / q fold to l / u / g; digits and symbols as LEET. */
function foldChar(ch) {
  const code = ch.codePointAt(0);
  let c = code < 0x80 ? ch.toLowerCase() : (WIDE[ch.toLowerCase().codePointAt(0)] || '');
  if (!c) return '';
  if (LEET[c]) c = LEET[c];
  if (!/[a-z]/.test(c)) return '';
  return c === 'i' ? 'l' : c === 'v' ? 'u' : c === 'q' ? 'g' : c;
}
/** The folded letters of a text, separators dropped, "ph" -> "f". */
export function filterFold(text) {
  let out = '';
  for (const ch of String(text)) out += foldChar(ch);
  return out.replace(/ph/g, 'f');
}
const collapse = (s, keep) => s.replace(/(.)\1+/g, (m, c) => (m.length > keep ? c.repeat(keep) : m));
/** The views a folded text is matched in: as folded, runs of 3+ cut to 2, every run cut to 1. */
const views = (f) => [f, collapse(f, 2), collapse(f, 1)];
const hasDouble = (f) => /(.)\1/.test(f);

const prep = (words) => words.map((w) => filterFold(w));
const SUB = prep(list.substring);
const WORD = prep(list.word);
const EXACT = prep(list.exact);
const R_WORD = prep(list.reservedWord);
const R_PREFIX = prep(list.reservedPrefix);

/** Does the folded text `f` contain / equal the folded list entry `e`? Entries with a doubled letter ("nigg") are never matched in the all-runs-cut view. */
const containsEntry = (f, e) => views(f).some((v, i) => (i < 2 || !hasDouble(e)) && v.includes(e));
const equalsEntry = (f, e) => views(f).some((v, i) => (i < 2 || !hasDouble(e)) && v === e);

/** The words of a name for the whole-word rules: split at separators and case changes; each also without digits around it. */
function wordForms(name) {
  return String(name)
    .replace(/([a-z])(?=[A-Z])/g, '$1 ')
    .split(/[^A-Za-z0-9\u00c0-\uffff]+/)
    .filter(Boolean)
    .flatMap((w) => {
      const bare = w.replace(/^\d+|\d+$/g, '');
      return bare && bare !== w ? [filterFold(w), filterFold(bare)] : [filterFold(w)];
    })
    .filter(Boolean);
}

/** The reason a name is not allowed by the shared filter: 'filtered' (rude), 'reserved' (impersonation) or null. Works on any text (the game calls the same logic). */
export function nameFilterReason(name, extra = []) {
  const whole = filterFold(name);
  if (!whole) return null;
  const words = wordForms(name);
  const reserved =
    R_WORD.some((r) => words.some((w) => equalsEntry(w, r) || equalsEntry(w, `${r}s`))) ||
    R_PREFIX.some((r) => words.some((w) => w.startsWith(r)));
  if (reserved || prep(list.reservedExact).some((r) => whole === r)) return 'reserved';
  const extraKeys = prep(extra).filter((e) => e.length >= 3);
  const rude =
    SUB.some((e) => containsEntry(whole, e)) ||
    extraKeys.some((e) => containsEntry(whole, e)) ||
    EXACT.some((e) => equalsEntry(whole, e)) ||
    WORD.some((e) => words.some((w) => equalsEntry(w, e) || equalsEntry(w, `${e}s`)));
  return rude ? 'filtered' : null;
}

/** Fold look-alikes so "Sally", "SaIly" and "sa11y" collide. Letters and digits only. */
export function foldKey(name) {
  const map = { 0: 'o', 1: 'l', 3: 'e', 4: 'a', 5: 's', 7: 't', 8: 'b', 9: 'g' };
  let out = '';
  for (const ch of name.toLowerCase()) {
    if (/[a-z0-9]/.test(ch)) out += map[ch] || ch;
    else if (ch === '$') out += 's';
    else if (ch === '@') out += 'a';
  }
  return out.replace(/i/g, 'l');
}

/** The words of a name as the filter sees them (folded): split at separators and at lower-to-upper case changes. */
export function nameWords(name) {
  return name
    .replace(/([a-z])(?=[A-Z])/g, '$1 ')
    .split(/[ ._-]+/)
    .filter(Boolean)
    .flatMap((w) => {
      const bare = w.replace(/^\d+|\d+$/g, '');
      return bare && bare !== w ? [foldKey(w), foldKey(bare)] : [foldKey(w)];
    });
}

/**
 * @param {unknown} raw
 * @param {{extraBlock?: string[]}} [opts]
 * @returns {{name: string, key: string}}
 */
export function validateName(raw, opts = {}) {
  if (typeof raw !== 'string') throw new ApiError(400, 'bad_name', 'name must be a string', { reason: 'type' });
  const name = raw.trim();
  const bad = (reason, msg) => new ApiError(400, 'bad_name', msg, { reason });
  if (name.length < NAME_MIN) throw bad('too_short', `name needs at least ${NAME_MIN} characters`);
  if (name.length > NAME_MAX) throw bad('too_long', `name has at most ${NAME_MAX} characters`);
  if (!/^[A-Za-z0-9 ._-]+$/.test(name)) throw bad('charset', 'letters, digits, space, dot, dash and underscore only');
  if (!/^[A-Za-z0-9]/.test(name) || !/[A-Za-z0-9]$/.test(name)) throw bad('edges', 'must start and end with a letter or digit');
  if (/ {2,}/.test(name)) throw bad('spaces', 'no double spaces');
  if ((name.match(/[A-Za-z0-9]/g) || []).length < 2) throw bad('too_few_alnum', 'needs at least two letters or digits');
  const key = foldKey(name);
  const why = nameFilterReason(name, opts.extraBlock || []);
  if (why === 'reserved') throw bad('reserved', 'that name is reserved');
  if (why) throw bad('filtered', 'that name is not allowed');
  return { name, key };
}
