import { pool } from "../config/database";
import { sendGeofenceExitAlert } from "./telegramNotifier";

export type GeofenceEventType = "exit" | "enter";

export interface GeofenceEvent {
    id: string;
    location_id: string;
    geofence_id: number;
    event_type: GeofenceEventType;
    occurred_at: string;
    created_at: string;
}

interface EvaluateGeofenceTransitionParams {
    animalId: number;
    geofenceId: number;
    locationId: string;
    insideGeofence: boolean;
    recordedAt: Date;
}

/**
 * RF27: solo confirma un traspaso cuando esta lectura y la inmediatamente
 * anterior del mismo animal coinciden en estar ambas dentro o ambas fuera.
 * RF18: solo registra el evento (exit/enter) una vez confirmado ese criterio,
 * y evita duplicar un "exit" mientras no exista un "enter" que lo resuelva
 * (el evento mas reciente del animal indica si ya hay un exit sin resolver).
 * Ambas busquedas se limitan a la misma geocerca: inside_geofence de poligonos
 * distintos no es comparable, asi que al cambiar de geocerca la evaluacion empieza de cero.
 */
export async function evaluateGeofenceTransition(
    params: EvaluateGeofenceTransitionParams
): Promise<GeofenceEvent | null> {
    const { animalId, geofenceId, locationId, insideGeofence, recordedAt } = params;

    const previousResult = await pool.query(
        `SELECT inside_geofence
         FROM locations
         WHERE animal_id = $1 AND geofence_id = $2 AND id <> $3
         ORDER BY recorded_at DESC, id DESC
         LIMIT 1`,
        [animalId, geofenceId, locationId]
    );

    if (previousResult.rowCount === 0) {
        return null;
    }

    const previousInsideGeofence: boolean = previousResult.rows[0].inside_geofence;
    if (previousInsideGeofence !== insideGeofence) {
        return null;
    }

    const latestEventResult = await pool.query(
        `SELECT ge.event_type
         FROM geofence_events ge
         JOIN locations l ON l.id = ge.location_id
         WHERE l.animal_id = $1 AND ge.geofence_id = $2
         ORDER BY ge.occurred_at DESC, ge.id DESC
         LIMIT 1`,
        [animalId, geofenceId]
    );
    const latestEventType: GeofenceEventType | null = latestEventResult.rows[0]?.event_type ?? null;

    let eventTypeToInsert: GeofenceEventType | null = null;
    if (!insideGeofence && latestEventType !== "exit") {
        eventTypeToInsert = "exit";
    } else if (insideGeofence && latestEventType === "exit") {
        eventTypeToInsert = "enter";
    }

    if (eventTypeToInsert === null) {
        return null;
    }

    const insertResult = await pool.query(
        `INSERT INTO geofence_events (location_id, geofence_id, event_type, occurred_at)
         VALUES ($1, $2, $3, $4)
         RETURNING *`,
        [locationId, geofenceId, eventTypeToInsert, recordedAt]
    );
    const event: GeofenceEvent = insertResult.rows[0];

    if (eventTypeToInsert === "exit") {
        const animalResult = await pool.query(
            "SELECT tag_number, name FROM animals WHERE id = $1",
            [animalId]
        );
        const animal = animalResult.rows[0];

        // Sin await: RNF1 exige que esto no retrase la respuesta de POST /locations.
        void sendGeofenceExitAlert({
            geofenceEventId: event.id,
            tagNumber: animal.tag_number,
            animalName: animal.name,
            occurredAt: recordedAt,
        });
    }

    return event;
}
