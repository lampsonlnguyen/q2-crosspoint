CrossPod Q2 Pod CARD APP (illustrated EPUB, Q2 ABI diagnostics)

INSTALLATION: Unzip crosspod-card-app.zip to the root of the Q2 SD card.
The exact file paths must be:
 /mnt/mmc/.crosspod/q2app
 /mnt/mmc/.crosspod/q2-crosspod    (POSIX shell launcher)
 /mnt/mmc/.crosspod/crosspod-engine (MIPS reader ELF)
 /mnt/mmc/Books/YourBook.epub

On supported Q2 Pod firmware, choose Library > CrossPod.
If your Q2 Pod version does not support manifest row=library, look under Games.
This is a standalone card-app launcher. DO NOT flash any firmware.
Q2 Pod does not need the optional CrossPod Books hook for this card app.

Opening the app without arguments shows a book selector. Turn the wheel,
press Centre to open, use wheel or left/right touchscreen to turn pages.
Back from reading returns to the CrossPod shelf; Back on the shelf exits.

Only EPUB files are currently handled. It lists the SD root Books folder and
one subfolder level. TXT books, JPEG, and complex EPUB layouts are not yet supported.
Current illustrated EPUB preview supports bounded non-interlaced RGB/RGBA PNGs.

Q2 device execution is NOT verified. The new build targets the Shanling Q2's
MIPS32r2 O32 FP64 hard-float, NaN-2008 and ABS-2008 ABI, with a strict ELF
pre-release check and static MIPS QEMU tests. Generic MIPS QEMU does not prove
physical device compatibility. Do NOT flash firmware or replace system libraries.

STARTUP DIAGNOSTICS:
Q2 Pod's game card executes .crosspod/q2-crosspod, a #!/bin/sh wrapper. The
wrapper logs to .crosspod/startup.log before attempting to exec crosspod-engine.
This log records kernel details, binary presence, free memory and reader stderr.
If the engine ELF fails to start, the shell's error is captured here.
The log is capped at approximately 64 KiB per attempt by rotating the previous
file to startup.previous.log. Logging failures do not prevent starting the app.
After testing on Q2, power off before removing the SD card, then send
.crosspod/startup.log and (if present) startup.previous.log. If no log exists,
then Q2 Pod did not reach the launcher (or the card was not writable); that
narrows the cause to the Q2 Pod fork/exec, script permission, or SD mount.
For direct terminal tests the reader is .crosspod/crosspod-engine, not the launcher.

This build uses CrossPoint's original EPUB ZIP/OPF/spine, ParsedText, TextBlock
and GfxRenderer; XHTML collection is provisional and can lose styling.

Reading-position resume (development branch): CrossPod now stores a small
per-book progress file under /mnt/mmc/.crosspod/ after an interactive page
turn. Reopening the same EPUB restores the last chapter and page. Progress
is ignored when the EPUB file size or modification time changes. The saved
page is clamped to the currently paginated chapter; this transitional format
is NOT CrossPoint's native progress.bin and is not KOReader sync compatible.
The CLI --validate, --render and --walk modes do not write progress.

TYPOGRAPHY MENU (experimental font builds):
While reading, press Centre or tap the top 45 pixels to open Reading appearance.
Turn the wheel to choose Font or Size; Centre cycles the selected value.
Back closes the menu. Choose Sans or Serif and 12, 14, 16, or 18 pt.
CrossPod repaginates the current chapter and saves your choices in
/mnt/mmc/.crosspod/typography.conf. The book's page number may shift when
font size changes; location anchoring is still a follow-up.

The card app CI installs the original CrossPoint NotoSansExtended and
NotoSerifExtended 12/14/16/18pt faces. It also installs upstream
NotoSansCJKJP (Japanese and common Chinese), Pretendard (Korean Hangul),
NotoSansDevanagari (Hindi, Marathi, Nepali), NotoSansBengali (Bengali),
NotoSansGujarati, NotoSansGurmukhi (Punjabi), NotoSansKannada,
NotoSansMalayalam, NotoSansOriya (Odia), NotoSansSinhala, NotoSansTamil,
NotoSansTelugu, NotoNaskhArabicUI (Arabic), and Alef (Hebrew). Each
fallback family is present at 12/14/16/18pt. All packages are downloaded
from the official CrossPoint fonts release with pinned SHA-256 checksums.
The optional script faces stay on SD until their glyphs are requested.

Fallback selection checks glyph coverage for each word and routes it to
the available original CrossPoint font covering the most missing glyphs.
The Sans/Serif preference remains primary. One font per word means mixed-script
words requiring multiple fallback faces can still be incomplete. The upstream
font inventory does not currently contain every language/script in Unicode,
and Devanagari/Arabic/other complex shaping fidelity is limited by the
underlying CrossPoint shaping engine. No Thai or Khmer fonts are in the
selected upstream release. For Chinese, the CJK font uses Japanese regional
Han forms; distinct Simplified Chinese glyph shapes are not guaranteed.
Font files do not imply support for every language or every ligature.

The expanded font assets increase the ZIP substantially. Install to SD root
with adequate space and retain the complete .crosspod/fonts folder.
An older ZIP with missing script files still opens using available fonts;
startup logs report the count of loaded script families. See the upstream
catalog at https://github.com/crosspoint-reader/crosspoint-fonts
