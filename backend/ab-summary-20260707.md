# Phase 0 A/B Summary - 2026-07-07

Inputs: `test-clips/rec_005.wav` through `rec_014.wav`, in the agreed 10-word order.

## Results

| Clip | Intended | Whisper transcript | Claude | GPT | Notes |
|---|---|---|---|---|---|
| rec_005 | en skjorta | En hårta. | hjärta | tårta | Speech-to-text miss was too far from target; both enrichers failed. |
| rec_006 | ett kvitto | Ett kvitto. | kvitto | kvitto | Both correct. |
| rec_007 | att skynda | att skynda. | skynda | skynda | Both correct. |
| rec_008 | att låtsas | att låtsas. | låtsas | låtsas | Both correct. |
| rec_009 | besviken | Besviken | besviken | besviken | Both correct. |
| rec_010 | krånglig | Krånglig. | krånglig | krånglig | Both correct; Claude included adjective forms. |
| rec_011 | förmodligen | Förmågorligen. | förmodligen | förmodligen | Both corrected the transcript. |
| rec_012 | lagom | Lagom. | lagom | lagom | Both useful; GPT's part of speech `adjektiv/adverb` is more complete. |
| rec_013 | skärgård | Sjögård | sjögård | skärgård | GPT recovered the intended word; Claude took the transcript literally. |
| rec_014 | Jag hörde ordet "rimlig" på jobbet | Jag hörde ordet rimligt på jobbet. | rimlig | rimlig | Both correct with context note. |

## Takeaways

- Whisper quality is the main bottleneck for hard phonetic misses (`skjorta` -> `hårta`).
- GPT currently looks stronger at recovering likely intended vocabulary from a plausible but wrong Swedish transcript (`sjögård` -> `skärgård`).
- Claude often gives slightly richer definitions/forms when the target word is already clear.
- The prompt should push harder for adjective forms and for cautious correction from near-homophones.

## Decision

Use Claude for Phase 0 enrichment. GPT had the strongest single rescue on `skärgård`, but Claude's card shape, definitions, and forms are the preferred default for the first production path. Keep `compare.py` available for future prompt/model checks.
