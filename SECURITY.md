# Security

lilJack runs other programs on your behalf (agents, shells, ffmpeg, yt-dlp)
and archives session transcripts. Treat it as you would any tool with shell
access.

- Report vulnerabilities through GitHub's private vulnerability reporting on
  this repository, not in a public issue.
- Transcripts are scrubbed of known credential literals before archiving when
  `LILJACK_SECRETS_SCRIPTS` points at a secrets source; without one, nothing is
  known to redact, so keep secrets out of agent sessions.
- The remote archive is optional and only used when you configure one.
  Its token is read from that config file or the environment and is never
  written to the repository.
- `tools/scan_private.sh` refuses commits that carry private paths, LAN
  addresses or credential shapes; CI runs it on every push.
