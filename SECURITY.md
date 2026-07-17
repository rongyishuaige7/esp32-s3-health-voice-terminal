# Security, privacy and safety boundary

This is an educational health-information voice prototype, not a medical device or production service.

## Credentials

- Real Wi-Fi, DeepSeek, XFYun, ASR and TTS credentials belong only in locally generated `sdkconfig`, which is ignored by Git.
- The providers' keys are separate credentials. This repository does not include or authorize a relay and does not translate one provider key into another.
- Kconfig secrets are compiled into firmware. Anyone with device or binary access may recover them; use scoped, low-quota, replaceable test credentials.
- Never publish `sdkconfig`, firmware built with real credentials, serial logs containing signed URLs, or XFYun WebSocket URLs with `authorization` query parameters.
- On suspected exposure, revoke provider keys and rotate Wi-Fi credentials. Merely deleting a later commit does not clean Git history.

## Health and privacy

- Voice can be sent to ASR, the transcript to a model provider, and the answer to TTS. Review every provider's current privacy, retention, regional and account terms before use.
- Do not provide identifiable health information, records, contact details or location data.
- The device has no consent workflow, identity control, tenant separation, encrypted local history, deletion workflow or clinical audit trail.
- The fixed prompt is not a safety control. Model output can be wrong, incomplete, overconfident or unsafe.
- Do not use the prototype for diagnosis, prescriptions, medication changes, triage, emergency response or unattended public access.

## Network and endpoints

- HTTPS/WSS clients use the ESP-IDF certificate bundle in the source paths, but user-configured endpoints and certificate compatibility remain the user's responsibility.
- Do not replace TLS verification with insecure certificate skipping.
- Custom endpoints can receive sensitive audio or text; use only endpoints you control and trust.
- Source error logging suppresses full generic HTTP response bodies, but provider SDK/event errors and local debugging can still expose context. Review logs before sharing.

Please report vulnerabilities through GitHub's private security advisory feature when possible. Do not include credentials, signed URLs, voice samples or health data in a public issue.
