import { Plugin } from "@opencode/plugin";
import { request } from "node:http";

const TTS_URL = "http://localhost:8080/v1/audio/speech";
const MAX_CHARS = 150;

type Pending = {
  messageID: string;
  parts: Map<number, string>;
};

function cleanText(input: string): string {
  const ticks = String.fromCharCode(96);
  let text = input
    .replace(new RegExp(ticks + "{3}[\\s\\S]*?" + ticks + "{3}", "g"), " ")
    .replace(/!\[[^\]]*\]\([^)]*\)/g, " ")
    .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
    .replace(new RegExp(ticks + "([^" + ticks + "]+)" + ticks, "g"), "$1")
    .replace(/https?:\/\/\S+/g, " ")
    .replace(/^\s{0,3}#{1,6}\s*/gm, "")
    .replace(/^\s*[-*+]\s+/gm, "")
    .replace(/\s+/g, " ")
    .trim();

  return Array.from(text).slice(0, MAX_CHARS).join("");
}

function sendTTS(text: string, sessionID: string, messageID: string): void {
  if (!text) return;

  const url = new URL(TTS_URL);
  const body = Buffer.from(JSON.stringify({
    input: text,
    voice: "default",
    speed: 1.0,
    session_id: sessionID,
    message_id: messageID,
  }));

  const req = request({
    hostname: url.hostname,
    port: Number(url.port || 80),
    path: url.pathname + url.search,
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "Content-Length": body.length,
    },
  }, (res) => {
    res.resume();
  });

  req.on("error", (error) => {
    console.error("[tts-notify] 请求失败:", error.message);
  });

  req.end(body);
}

export default Plugin.define({
  id: "tts-notify",
  setup(ctx) {
    const controller = new AbortController();
    const sessions = new Map<string, Pending>();

    void (async () => {
      try {
        for await (const event of ctx.event.subscribe({
          types: [
            "session.step.started",
            "session.text.ended",
            "session.execution.succeeded",
            "session.execution.failed",
            "session.execution.interrupted",
          ],
          signal: controller.signal,
        })) {
          const data = (event as any).data;
          if (!data?.sessionID) continue;
          const sessionID = data.sessionID;

          if (event.type === "session.step.started") {
            sessions.set(sessionID, {
              messageID: data.assistantMessageID || "",
              parts: new Map(),
            });
            continue;
          }

          if (event.type === "session.text.ended") {
            let state = sessions.get(sessionID);
            if (!state || state.messageID !== data.assistantMessageID) {
              state = {
                messageID: data.assistantMessageID || "",
                parts: new Map(),
              };
              sessions.set(sessionID, state);
            }
            if (typeof data.ordinal === "number" && typeof data.text === "string") {
              state.parts.set(data.ordinal, data.text);
            }
            continue;
          }

          if (event.type === "session.execution.succeeded") {
            const state = sessions.get(sessionID);
            sessions.delete(sessionID);
            if (!state || state.parts.size === 0 || !state.messageID) continue;

            const finalText = [...state.parts.entries()]
              .sort((a, b) => a[0] - b[0])
              .map(([, text]) => text)
              .join("\n");

            const speech = cleanText(finalText);
            if (speech) sendTTS(speech, sessionID, state.messageID);
            continue;
          }

          if (
            event.type === "session.execution.failed" ||
            event.type === "session.execution.interrupted"
          ) {
            sessions.delete(sessionID);
          }
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          console.error("[tts-notify] 事件监听异常:", error);
        }
      }
    })();

    return () => {
      controller.abort();
      sessions.clear();
    };
  },
});
