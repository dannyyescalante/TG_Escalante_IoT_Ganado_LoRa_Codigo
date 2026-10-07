import { pool } from "../config/database";

const TELEGRAM_API_TIMEOUT_MS = 10_000;

interface GeofenceExitAlertParams {
    geofenceEventId: string;
    tagNumber: string;
    animalName: string | null;
    occurredAt: Date;
}

interface AlertAttempt {
    status: "sent" | "failed";
    responseCode: number | null;
    telegramMessageId: number | null;
    errorDetail: "timeout" | "network" | "http_error" | null;
}

// Un registro por intento en alerts: RNF1 se mide como alerts.sent_at - geofence_events.created_at.
// Si no se puede guardar, solo se avisa en el log: nunca afecta la ingesta de ubicaciones.
async function recordAlertAttempt(geofenceEventId: string, attempt: AlertAttempt): Promise<void> {
    try {
        await pool.query(
            `INSERT INTO alerts (geofence_event_id, channel, status, response_code, telegram_message_id,
                                 error_detail, sent_at)
             VALUES ($1, 'telegram', $2, $3, $4, $5, now())`,
            [geofenceEventId, attempt.status, attempt.responseCode, attempt.telegramMessageId, attempt.errorDetail]
        );
    } catch (err) {
        console.error("No se pudo registrar el intento de alerta en la tabla alerts:", (err as Error).message);
    }
}

// Nunca lanza: una alerta de Telegram fallida o lenta no debe tumbar ni
// retrasar la respuesta de POST /locations (el evento ya quedo guardado
// en geofence_events pase lo que pase aqui).
export async function sendGeofenceExitAlert(params: GeofenceExitAlertParams): Promise<void> {
    const { geofenceEventId, tagNumber, animalName, occurredAt } = params;

    const botToken = process.env.TELEGRAM_BOT_TOKEN;
    const chatId = process.env.TELEGRAM_CHAT_ID;

    if (!botToken || !chatId) {
        // Sin credenciales no hay intento, y por lo tanto no hay registro en alerts.
        console.warn("TELEGRAM_BOT_TOKEN o TELEGRAM_CHAT_ID no estan configurados; no se envio la alerta.");
        return;
    }

    const hora = occurredAt.toLocaleString("es-VE", { timeZone: "America/Caracas" });
    const animalLabel = animalName ? `${tagNumber} (${animalName})` : tagNumber;
    const text = `Alerta de geocerca\nAnimal: ${animalLabel}\nHora: ${hora}\nSalio de la geocerca.`;

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), TELEGRAM_API_TIMEOUT_MS);

    let attempt: AlertAttempt;
    try {
        const response = await fetch(`https://api.telegram.org/bot${botToken}/sendMessage`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ chat_id: chatId, text }),
            signal: controller.signal,
        });
        const body = await response.text();

        let messageId: number | null = null;
        try {
            messageId = JSON.parse(body)?.result?.message_id ?? null;
        } catch {
            messageId = null;
        }

        if (response.ok && messageId !== null) {
            attempt = { status: "sent", responseCode: response.status, telegramMessageId: messageId, errorDetail: null };
        } else {
            console.error(`Telegram respondio con error (${response.status}): ${body}`);
            attempt = { status: "failed", responseCode: response.status, telegramMessageId: null, errorDetail: "http_error" };
        }
    } catch (err) {
        const timedOut = (err as Error).name === "AbortError";
        console.error("No se pudo enviar la alerta de Telegram:", (err as Error).message);
        attempt = {
            status: "failed",
            responseCode: null,
            telegramMessageId: null,
            errorDetail: timedOut ? "timeout" : "network",
        };
    } finally {
        clearTimeout(timeout);
    }

    await recordAlertAttempt(geofenceEventId, attempt);
}
