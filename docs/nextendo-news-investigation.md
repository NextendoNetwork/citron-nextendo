# Native Nextendo News: investigation (2026-10-15)

Status: **not implemented end to end; no new runtime package released**.
The owner selected Nextendo/Citron announcements and supplied a console screenshot
showing the Nextendo News launch, Super Mario 3D World online, and Tetris 99.

## Confirmed client gaps

- INewsDatabaseService::Count, CountWithKey, GetList and GetListV1 always return zero.
- INewsDataService::Read and GetSize always return zero.
- INewsService::RequestImmediateReception does not fetch content.
- IDownloadContext currently signals an empty completion without a download.
- Legacy firmware 1.0.0 INewsService commands 30900, 30901 and 30902 returned success
  without the required arrival-event, data-service and database-service objects.
  This patch corrects those three factory responses. Modern firmware uses the
  IServiceCreator factories already present; this correction alone does not make
  modern HOME News display content.

## Confirmed public transport

Prelude redirects bcat-topics-lp1.cdn.nintendo.net to the configured Nextendo
server. Public GET probes against Citron's default configured Nextendo server on
2026-10-15 returned HTTP 200 application/octet-stream for:

- /api/nx/v1/titles/0100000000001000/topics?l=en-US&c%5B%5D=US (335 bytes)
- /api/nx/v1/topics/catalog (485 bytes)

Both responses start with the BCAT container magic and declare AES-256-CTR.
The secret-data selector is 0x18; the older public format documentation describes
only sixteen retail selectors. Do not assume an empty passphrase, invent keys,
or treat ciphertext as MessagePack. No console passphrase, key file, personal
account data or authenticated response was read or exported for these probes.
The diagnostic used a direct Nextendo routing override and relaxed certificate
verification; that is not a production transport implementation or validation.
The public website origin returned 403 for these routes and must not be assumed
to expose a public JSON announcements feed.

## Integration still needed

Obtain the current Nextendo News container/crypto contract or a public, documented
unpacked announcements feed from its maintainer. A public article fixture plus
its topic, index and image metadata would permit a deterministic client test.
Implement shared article storage, filtering/order, read/bookmark flags, bounded
background reception, persistence and arrival notification. Network waits must
not block the emulated IPC service thread. Preserve cached articles on failure;
use the active Nextendo connection settings and verified transport.

Acceptance requires native Featured/Latest cards, the HOME sidebar, opening an
article with its image, returning to HOME, and offline-cache/error behavior.
No such acceptance result is claimed by this investigation.

## Validation of this incremental patch

- Actual Windows production news_service.cpp compiled successfully with the
  existing Clang build command and CMIF serialization templates.
- Original developer source files were restored after compilation.
- The running beta54.3.5 executable and distributed archives were not replaced.
- git diff --check passed.

## Protocol references

- https://switchbrew.org/wiki/BCAT_services
- https://switchbrew.org/wiki/BCAT_Content_Container
- https://github.com/kinnay/NintendoClients/wiki/BCAT-Servers
- Prelude-Nextendo source/nextendo_news.c and source/nextendo_apply.c

## Follow-up: official Ryujinx-Nextendo v1.9.3 reference located

The owner's supplied Nextendo-1.9.3 ZIP matches the official release hash.
The corresponding public source now provides a concrete implementation in
src/Ryujinx.HLE/HOS/Services/News/NextendoNews.cs and INewsDatabaseService.cs.
It documents topic reception, decryption, MessagePack ingestion and native queries.
The earlier lack of a reference implementation is therefore resolved.

That implementation obtains its news-specific runtime parameters from
nextendo_news_key.json in the program or application-data directory. The supplied
ZIP does not contain that file as a separate entry. No private data directory,
passphrase or key material was inspected. The runtime parameter delivery contract
still needs maintainer confirmation before porting reception. Do not add guessed
parameters or extract unrelated keys from a user's configuration.

Reference:
https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/v1.9.3/src/Ryujinx.HLE/HOS/Services/News/NextendoNews.cs
