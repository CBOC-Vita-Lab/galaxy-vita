// One redactor for EVERY piece of upstream text (the relay API's answers, the analytics API's errors, a fetch failure) that can reach an answer, a log
// line or the admin stats. It keeps no body: callers pass the text in and get at most 120 characters back, with
//   - the exact values of TURN_KEY_API_TOKEN, TURN_USAGE_TOKEN, TURN_KEY_ID and TURN_ACCOUNT_ID replaced by [REDACTED],
//   - e-mail addresses -> [EMAIL], runs of 32+ hex digits -> [hex], any other run of 32+ of [A-Za-z0-9_.~+/=-] (token-shaped) -> [token].
// Control characters (except newline and tab) are removed. The masking comes BEFORE the cut to 120, so a secret that would straddle the cut is already gone.

export const REDACT_MAX = 120;
const MIN_SECRET = 4; // a shorter "secret" would only mangle ordinary words

/** @param {{keyId?: string, apiToken?: string, usageToken?: string, accountId?: string}} t cfg.turn */
export function secretsOf(t) {
  return [t && t.apiToken, t && t.usageToken, t && t.keyId, t && t.accountId].filter((v) => typeof v === 'string' && v.length >= MIN_SECRET);
}

/** @param {unknown} text @param {string[]} secrets */
export function redact(text, secrets = [], max = REDACT_MAX) {
  // control characters (except newline and tab, which the whitespace squeeze below turns into spaces) are dropped first: no escape sequence reaches a console
  let s = String(text ?? '').replace(/[\u0000-\u0008\u000B-\u001F\u007F]/g, '');
  for (const v of [...secrets].sort((a, b) => b.length - a.length)) if (v && v.length >= MIN_SECRET) s = s.split(v).join('[REDACTED]');
  s = s.replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, '[EMAIL]')
    .replace(/[0-9A-Fa-f]{32,}/g, '[hex]')
    .replace(/[A-Za-z0-9_.~+/=-]{32,}/g, '[token]')
    .replace(/\s+/g, ' ')
    .trim();
  return s.slice(0, max);
}

/** A redactor bound to the TURN config's secrets. */
export const redactorFor = (t) => {
  const secrets = secretsOf(t);
  return (text, max) => redact(text, secrets, max);
};
