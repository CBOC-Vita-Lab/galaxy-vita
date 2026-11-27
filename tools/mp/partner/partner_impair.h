// Galaxy 3.0 practice partner: the loss / delay the partner puts on its own traffic (partner.cpp sets it, partner_udp.cpp applies it
// to the Play-by-Address socket; the Same Wi-Fi link takes the same numbers through SMG_NET_DROP / DUP / DELAY_MS / JITTER_MS).
#ifndef PARTNER_IMPAIR_H
#define PARTNER_IMPAIR_H
struct PartnerImpair {
    double lossPct = 0, dupPct = 0;
    unsigned delayMs = 0, jitterMs = 0;
    bool any() const { return lossPct > 0 || dupPct > 0 || delayMs || jitterMs; }
};
extern PartnerImpair g_partnerImpair;
#endif
