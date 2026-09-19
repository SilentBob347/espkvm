# Notifications

The device can say something happened while nobody was watching: a watched
phrase appeared on the screen, the screen went blank, a dashcam clip was saved,
the tailnet key is about to run out. It sends that to Telegram, to a webhook, or
to both. Everything here is set in the console under Settings &rarr;
Notifications; `POST /api/v1/notify/test` sends a test one and
`GET /api/v1/notify/status` says how the last one went.

## Telegram

Put a bot token and a chat id in the settings. **Find chats** lists the chats
the bot can already write to, so the id does not have to be looked up by hand.
With the MJPEG codec a screenshot is attached; with H.264 the frame cannot be
handed over as a JPEG, so the message goes without one. A dashcam clip under
50 MB is uploaded as a video that plays in the chat; a bigger one is named
instead.

## Webhook

A plain `POST` to the URL in the settings, `Content-Type: application/json`:

```json
{
  "title": "Screen text",
  "message": "The phrase \"Press F1 to continue\" is on the screen.",
  "device": "espkvm",
  "log": "the last few kilobytes of the device log"
}
```

| Field | Always there | What it is |
| --- | --- | --- |
| `title` | yes | what kind of event it is, a few words |
| `message` | yes | the sentence a person reads |
| `device` | yes | the device's hostname, so several devices can share one endpoint |
| `log` | no | the tail of the device log, only while **Attach the log tail** is on |

Nothing is uploaded to a webhook: a clip is named in `message` as the file on
the card ("... - saved on the card as VIDEO/20260917-140322-event.mp4"), and a
screenshot is not attached at all. Any 2xx answer counts as delivered; anything
else is reported in `GET /api/v1/notify/status`.

The body is small enough for the usual receivers - an n8n or Node-RED webhook
node, a Home Assistant `webhook` trigger, an ntfy or Gotify endpoint behind a
small script, a Slack or Discord relay. There is no signature or shared secret,
so put the endpoint somewhere only your network can reach it, or give the URL a
long unguessable path.

## The tailnet key

Tailscale gives a node key six months at most, and when it lapses the device
drops off the tailnet until someone authorises it again. The device learns the
date from the control plane, shows it in Settings &rarr; VPN, reports it in
`GET /api/v1/system/info` as `ts.keyExpiry` (Unix epoch seconds, 0 when the
tailnet has key expiry switched off), and sends a notification a set number of
days before - **Warn before the key runs out** in the VPN settings, 14 days by
default, 0 to switch it off. The warning needs the clock in sync, which is what
the time settings under System are for.
