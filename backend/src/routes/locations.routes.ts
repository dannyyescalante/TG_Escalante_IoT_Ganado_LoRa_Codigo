import { Router, Request, Response } from "express";
import { pool } from "../config/database";
import { evaluateGeofenceTransition } from "../services/geofenceEvents";

export const locationsRouter = Router();

function isNonEmptyString(value: unknown): value is string {
    return typeof value === "string" && value.trim().length > 0;
}

function isValidLatitude(value: unknown): value is number {
    return typeof value === "number" && Number.isFinite(value) && value >= -90 && value <= 90;
}

function isValidLongitude(value: unknown): value is number {
    return typeof value === "number" && Number.isFinite(value) && value >= -180 && value <= 180;
}

function parsePositiveInt(raw: string): number | null {
    if (!/^\d+$/.test(raw)) return null;
    return Number(raw);
}

// POST /locations
locationsRouter.post("/", async (req: Request, res: Response) => {
    const { collar_device_id, lat, lon, timestamp } = req.body ?? {};

    const errors: string[] = [];
    if (!isNonEmptyString(collar_device_id)) {
        errors.push("collar_device_id es requerido y debe ser un texto no vacio.");
    }
    if (!isValidLatitude(lat)) errors.push("lat es requerido y debe ser un numero entre -90 y 90.");
    if (!isValidLongitude(lon)) errors.push("lon es requerido y debe ser un numero entre -180 y 180.");
    if (!isNonEmptyString(timestamp) || Number.isNaN(Date.parse(timestamp))) {
        errors.push("timestamp es requerido y debe ser una fecha valida (ISO 8601).");
    }

    if (errors.length > 0) {
        return res.status(400).json({ status: "error", message: "Datos invalidos.", errors });
    }

    try {
        const animalResult = await pool.query(
            "SELECT id FROM animals WHERE collar_device_id = $1",
            [collar_device_id.trim()]
        );

        if (animalResult.rowCount === 0) {
            return res.status(404).json({
                status: "error",
                message: `No existe ningun animal registrado con collar_device_id "${collar_device_id}".`,
            });
        }

        const animalId = animalResult.rows[0].id;

        // ST_Contains y el INSERT van en una sola sentencia (INSERT ... SELECT ... FROM geofence)
        // para que el resultado de inside_geofence corresponda exactamente a la fila insertada,
        // sin una lectura previa separada de la geocerca.
        const insertResult = await pool.query(
            `WITH inserted AS (
                INSERT INTO locations (animal_id, geom, recorded_at, inside_geofence)
                SELECT $1,
                       ST_SetSRID(ST_MakePoint($2, $3), 4326),
                       $4,
                       ST_Contains(geofence.geom, ST_SetSRID(ST_MakePoint($2, $3), 4326))
                FROM geofence
                ORDER BY id
                LIMIT 1
                RETURNING *
             )
             SELECT id, animal_id, ST_Y(geom) AS lat, ST_X(geom) AS lon,
                    recorded_at, received_at, inside_geofence, created_at
             FROM inserted`,
            [animalId, lon, lat, timestamp]
        );

        if (insertResult.rowCount === 0) {
            return res.status(500).json({
                status: "error",
                message: "No hay ninguna geocerca configurada en el sistema.",
            });
        }

        const location = insertResult.rows[0];

        const geofenceEvent = await evaluateGeofenceTransition({
            animalId: location.animal_id,
            locationId: location.id,
            insideGeofence: location.inside_geofence,
            recordedAt: location.recorded_at,
        });

        res.status(201).json({ ...location, geofence_event: geofenceEvent });
    } catch (err) {
        res.status(500).json({ status: "error", message: (err as Error).message });
    }
});

// GET /locations/:animalId
locationsRouter.get("/:animalId", async (req: Request, res: Response) => {
    const animalId = parsePositiveInt(req.params.animalId);
    if (animalId === null) {
        return res.status(400).json({ status: "error", message: "El animalId debe ser un numero entero positivo." });
    }

    let limit = 100;
    if (req.query.limit !== undefined) {
        const parsedLimit = parsePositiveInt(String(req.query.limit));
        if (parsedLimit === null) {
            return res.status(400).json({ status: "error", message: "limit debe ser un numero entero positivo." });
        }
        limit = parsedLimit;
    }

    try {
        const animalResult = await pool.query("SELECT id FROM animals WHERE id = $1", [animalId]);
        if (animalResult.rowCount === 0) {
            return res.status(404).json({ status: "error", message: `No existe un animal con id ${animalId}.` });
        }

        const locationsResult = await pool.query(
            `SELECT id, animal_id, ST_Y(geom) AS lat, ST_X(geom) AS lon,
                    recorded_at, received_at, inside_geofence, created_at
             FROM locations
             WHERE animal_id = $1
             ORDER BY recorded_at DESC, id DESC
             LIMIT $2`,
            [animalId, limit]
        );

        res.json(locationsResult.rows);
    } catch (err) {
        res.status(500).json({ status: "error", message: (err as Error).message });
    }
});
