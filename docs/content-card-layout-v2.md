# Content-card layout v2

PDCT stays512 bytes. Existing v1 cards and default text-first Swift output are
byte-for-byte unchanged. V2 has header byte4=2 and layout byte491=1 (image first)
or2 (side by side). All other v1 header/text/path constraints remain, bytes492
through507 must be zero, and CRC32 at508 covers bytes0–507. V2 layout0 and unknown
values are rejected; use v1 for the default. No scripts, input commands or new
assets are introduced. Layout changes require no firmware image once this
decoder/runtime is installed. The frozen installed reader is not claimed to
support this extension.

PDCM v1 gains required-capability bit4 (`CAP_CARD_LAYOUT`). Existing card/image
bits1/2 are unchanged. The manifest parser permits the bit only when supported;
full revision verification requires it exactly when at least one decoded card
has a nondefault layout. A missing or superfluous layout bit refuses activation.
State responses advertise capabilities7 in the new runtime. Swift checks all
required bits before manifest staging, and persists them in activation intent.
Old capability3 readers remain usable for default cards; alternate layouts fail
before transfer, not after an upload. Legacy manifests keep their bytes/hashes.

## Shared drawing rules

The existing ContentPageRenderer remains shared by native device content views
and the host C ABI. Title, footer labels, font, orientation and safe insets are
unchanged. The body consists of required text, optional context and one image:

- Text first: existing text/context limits (6/4 lines), then image in remaining
  body area.
- Image first: image occupies one third of the available post-title image box;
  existing gap follows it, then text/context use the remaining lines.
- Side by side: text/context occupy the left half of the post-title width after
  subtracting the existing gap; image occupies the right half and body height.
- Without an image, all layouts use the ordinary full-width text flow. A tiny
  or invalid geometry refuses drawing, never overlaps the fixed footer.

All rectangles use oriented renderer metrics. Existing PBM fitting preserves
aspect ratio; its HAL-backed image painter is unchanged. No new framebuffer,
heap allocation, task, server, network request or OTA action is added. Each
ContentCard gains one layout byte (native struct padding may affect total size);
existing revision/view workspace static bounds continue to apply. Bad image
reads still fail the entire paint and cannot become a completed-frame receipt.

The host C ABI remains1: its512-byte document argument is unchanged and the
new pinned artifact accepts both PDCT versions. App imports must use the new
source-bound artifact, not claim an older artifact supports v2.

Swift stores nondefault-layout drafts as schema2 so older apps refuse them
instead of silently discarding layout; missing layout in old schema1 JSON
decodes as text first. Save and Apply remain distinct. Local preview gets the
same encoded card as deployment, with its existing explicit Base/English/font
reference label (not a capture of connected-reader settings).

## Golden fixtures and acceptance

Using the Korean fixture from content-card-v1.md, v2 image-first trailer is
`e94535c5`, SHA256 `2241823c074d4eb1237d095728a19dac8164a133df88a1604539e55e9ffb42a7`.
Side-by-side trailer is `6d1eaf96`, SHA256
`29bb83d25205c6d23a9af2047e301f8832e07a9b0e4d996dcea17c9bb6ec04e5`.
Both repositories pin these independently computed fixtures.

Host coverage includes decode/refusal, manifest capability mismatch, seal→activate
→load/view propagation, and real-font Cached/BoundedUI plus C ABI comparisons
across both panel geometries and all rotations. Swift coverage includes saved
draft reload, capability refusal before upload, reused image skipping and actual
layout-dependent pixels through the imported host library. These are local
checks, not physical panel, SD power-cut or X3/X4 acceptance evidence.

This extends one card-detail surface. Editable arbitrary row/column trees,
home/list surfaces, resource/font authoring, authenticated writes and complete
session ownership remain separate requirements; three presets do not complete
the full UI studio.
