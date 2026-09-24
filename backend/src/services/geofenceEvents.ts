import { pool } from "../config/database";

export type GeofenceEventType = "exit" | "enter";

export interface GeofenceEvent {
    id: string;
    animal_id: number;
    location_id: string;
    event_type: GeofenceEventType;
    occurred_at: string;
    created_at: string;
}

interface EvaluateGeofenceTransitionParams {
    animalId: number;
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
 */
export async function evaluateGeofenceTransition(
    params: EvaluateGeofenceTransitionParams
): Promise<GeofenceEvent | null> {
    const { animalId, locationId, insideGeofence, recordedAt } = params;

    const previousResult = await pool.query(
        `SELECT inside_geofence
         FROM locations
         WHERE animal_id = $1 AND id <> $2
         ORDER BY recorded_at DESC, id DESC
         LIMIT 1`,
        [animalId, locationId]
    );

    if (previousResult.rowCount === 0) {
        return null;
    }

    const previousInsideGeofence: boolean = previousResult.rows[0].inside_geofence;
    if (previousInsideGeofence !== insideGeofence) {
        return null;
    }

    const latestEventResult = await pool.query(
        `SELECT event_type
         FROM geofence_events
         WHERE animal_id = $1
         ORDER BY occurred_at DESC, id DESC
         LIMIT 1`,
        [animalId]
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
        `INSERT INTO geofence_events (animal_id, location_id, event_type, occurred_at)
         VALUES ($1, $2, $3, $4)
         RETURNING *`,
        [animalId, locationId, eventTypeToInsert, recordedAt]
    );

    return insertResult.rows[0];
}
