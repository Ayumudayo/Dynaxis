Place compiled chat hook plugin binaries in this directory for Docker stack tests.

Expected mount:
- `docker/stack/plugins` -> `/app/plugins` (read-only)

Example file names:
- `10_chat_hook_sample.so`
- `20_chat_hook_tag.so`

If this directory is empty, native chat-hook plugin loading is skipped.
