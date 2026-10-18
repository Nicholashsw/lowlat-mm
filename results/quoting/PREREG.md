# A better quoting rule: pre-registration

Written on 19 October 2026, before any run of the new rule.

## The rule

The baseline is the existing market maker: quotes two ticks either side of an
inventory-skewed mid, requoted on almost every top-of-book change. The new rule, in the
OCaml replay:

- **Fair value** is the microprice, (bid x ask size + ask x bid size) / (bid size + ask size).
- **Quotes join the touch**: the bid at the best bid, the offer at the best offer.
- **Requotes only when fair value has moved at least one tick** since the quotes were
  placed. A side with no resting order (after a fill or a pull) is placed straight away.
- **Inventory**: both quotes move k ticks away from the side that adds exposure for every
  10 contracts held (down when long, up when short), never crossing the book, and the side
  that would take the position beyond L contracts is not quoted.
- **Adverse-selection guard**: with imbalance (bid size - ask size) / (bid size + ask size),
  the offer is pulled when imbalance is above theta and the bid when it is below -theta.

Everything else is unchanged: data/es.ini (10 contracts a quote, latencies, risk limits).

## Tuning and test

- **Grid**: k in {0, 1, 2}, L in {10, 20, 40}, theta in {0.3, 0.5, 0.7}: 27 combinations.
- **Training**: the first 10 sessions, 18 September to 1 October 2026. The combination with
  the highest mean daily PnL under the queue-behind fill rule is chosen. Ties go to the
  smaller k, then the smaller L, then the larger theta.
- **Test**: the chosen combination is run once on the last 11 sessions, 2 to 16 October
  2026, under all four fill rules, next to the baseline. Cached feeds only.

## Hypothesis

On the 11 test sessions the new rule earns a higher mean daily PnL than the baseline under
queue-behind. Expected: no, or not significantly.

## Tests counted

- The 27 grid combinations (selection).
- Test sessions, per fill rule: mean daily PnL of the new rule (4), of the baseline (4),
  and of new minus baseline, paired by day (4).
- Test sessions, per rule and fill rule: the slope of daily PnL on ES's open-to-close move
  (8).

47 tests. Bonferroni: p < 0.05 / 47 = 0.0011.

## Porting to C++

Only if, on the test sessions under queue-behind, the new rule's mean markout per fill
beats the baseline's at all of 1 s, 1 m and 5 m. The 0 ms markout is not part of the test,
because quoting at the touch mechanically captures less at the moment of the fill than
quoting behind it. Any port keeps crosscheck.sh identical on the touch rule.
