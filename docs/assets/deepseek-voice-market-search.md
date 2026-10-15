<p align="right">
  <strong>English</strong> · <a href="deepseek-voice-market-search.zh_CN.md">简体中文</a>
</p>

# Marketplace Keyword Trace: DeepSeek Voice Play

Date: 2026-10-09  
Status: **Exact listing not yet confirmed**

## Marketplace entry point and search result

- Official marketplace: [AI Passport Play Community](https://ai-passport.folotoy.cn/plays/)
- Official product page: [AI Passport](https://ai-passport.folotoy.cn/)
- Public source candidate: [FoloToy/folo-ai-passport-xiaozhi](https://github.com/FoloToy/folo-ai-passport-xiaozhi)

The marketplace's public index shows 596 plays and lists the XiaoZhi AI chatbot by FOLOTOY in its download ranking. The indexed page excerpt does not show a play whose title or description contains the exact Chinese keyword. Exact-phrase searches of publicly indexed pages have not surfaced a verifiable marketplace detail page with that phrase.

This does **not** prove the play does not exist: community search is dynamically loaded, and the current public index is insufficient to confirm all results. The correct status is “not confirmed in indexed results,” not “confirmed absent.”

## Why the source candidate is not yet the target firmware

FoloToy's public repository README describes XiaoZhi as a voice-interaction entry that can use Qwen / DeepSeek models. Its source can be inspected for configuration, NVS, asset partitions and the audio pipeline, but the evidence chain is not yet closed:

1. Exact marketplace listing URL / play ID containing “DeepSeek voice”;
2. The installable firmware file for that listing and its SHA-256;
3. The source version or build commit corresponding to that binary.

Therefore, NVS/assets behavior in this repository must not be presented as verified behavior of the exact marketplace play.

## Storage behavior currently visible in the source candidate only

The public AI Passport / XiaoZhi configuration and 8 MiB partition table point to shared NVS plus an assets resource partition; no dedicated FAT recording archive was found. Settings use NVS key-value APIs, resources are read/mapped through the assets partition label, and voice interaction mainly follows a network audio pipeline. A provisional hypothesis is that this candidate is better suited to NVS/resource-partition comparison than as the first acceptance target for a user-file DATA lifecycle.

These are source-candidate findings, not a final conclusion about the marketplace listing. Binary download, hash and source-build mapping are incomplete. Voice capability alone does not prove that audio or transcripts are persisted to device flash.

## Hard completion criteria

- Search the official community UI for the exact Chinese keyword; record matching title, detail URL, creator, version/update time and downloads.
- Follow the detail page's source-repository link if available; otherwise record that source is not published.
- Download the actual firmware from the detail page and record original URL, size, SHA-256 and acquisition time.
- Use the matching source version to inspect partition tables, mount APIs, NVS keys, file create/delete paths and reboot recovery.
- If no firmware download or source-to-listing evidence exists, keep the task BLOCKED; do not substitute a similar project.

## Related automation tests

See the [mobile embedded installer E2E design](mobile-page-storage-e2e-design.md) for real-device space management and child-firmware install/uninstall. Market-firmware analysis establishes real product data behavior; the synthetic child firmware verifies installer and DATA-carve infrastructure. Neither substitutes for the other.
