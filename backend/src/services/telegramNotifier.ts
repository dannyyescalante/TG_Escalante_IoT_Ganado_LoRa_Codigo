const TELEGRAM_API_TIMEOUT_MS = 10_000;

interface GeofenceExitAlertParams {
    tagNumber: string;
    animalName: string | null;
    occurredAt: Date;
}

// Nunca lanza: una alerta de Telegram fallida o lenta no debe tumbar ni
// retrasar la respuesta de POST /locations (el evento ya quedo guardado
// en geofence_events pase lo que pase aqui).
export async function sendGeofenceExitAlert(params: GeofenceExitAlertParams): Promise<void> {
    const { tagNumber, animalName, occurredAt } = params;

    const botToken = process.env.TELEGRAM_BOT_TOKEN;
    const chatId = process.env.TELEGRAM_CHAT_ID;

    if (!botToken || !chatId) {
        console.warn("TELEGRAM_BOT_TOKEN o TELEGRAM_CHAT_ID no estan configurados; no se envio la alerta.");
        return;
    }

    const hora = occurredAt.toLocaleString("es-VE", { timeZone: "America/Caracas" });
    const animalLabel = animalName ? `${tagNumber} (${animalName})` : tagNumber;
    const text = `Alerta de geocerca\nAnimal: ${animalLabel}\nHora: ${hora}\nSalio de la geocerca.`;

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), TELEGRAM_API_TIMEOUT_MS);

    try {
        const response = await fetch(`https://api.telegram.org/bot${botToken}/sendMessage`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ chat_id: chatId, text }),
            signal: controller.signal,
        });

        if (!response.ok) {
            const body = await response.text();
            console.error(`Telegram respondio con error (${response.status}): ${body}`);
        }
    } catch (err) {
        console.error("No se pudo enviar la alerta de Telegram:", (err as Error).message);
    } finally {
        clearTimeout(timeout);
    }
}
