import { Router, Request, Response } from "express";
import { pool } from "../config/database";
import { sendInternalError } from "../http/errors";
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
    // Sin ceros a la izquierda y mayor que 0: "0" (o "00") no es un entero positivo.
    if (!/^[1-9]\d*$/.test(raw)) return null;
    return Number(raw);
}

// Campos opcionales: undefined o null = no enviado; si viene, debe estar en rango.
function isOptionalNumberInRange(value: unknown, min: number, max: number, integer: boolean): boolean {
    if (value === undefined || value === null) return true;
    if (typeof value !== "number" || !Number.isFinite(value)) return false;
    if (integer && !Number.isInteger(value)) return false;
    return value >= min && value <= max;
}

// Margen para la diferencia entre la hora GPS del collar y el reloj del PC del backend.
// Una lectura "del futuro" pasaria a ser la mas reciente y las reales se verian como atrasadas.
const MAX_CLOCK_AHEAD_MS = 5 * 60 * 1000;

const LOCATION_COLUMNS = `id, animal_id, geofence_id, ST_Y(geom) AS lat, ST_X(geom) AS lon,
    recorded_at, received_at, inside_geofence, battery_percent, satellites, packet_seq,
    rssi, snr, created_at`;

// POST /locations
locationsRouter.post("/", async (req: Request, res: Response) => {
    const { collar_device_id, lat, lon, timestamp, battery_percent, satellites, packet_seq, rssi, snr } =
        req.body ?? {};

    const errors: string[] = [];
    if (!isNonEmptyString(collar_device_id)) {
        errors.push("collar_device_id es requerido y debe ser un texto no vacio.");
    }
    if (!isValidLatitude(lat)) errors.push("lat es requerido y debe ser un numero entre -90 y 90.");
    if (!isValidLongitude(lon)) errors.push("lon es requerido y debe ser un numero entre -180 y 180.");
    if (!isNonEmptyString(timestamp) || Number.isNaN(Date.parse(timestamp))) {
        errors.push("timestamp es requerido y debe ser una fecha valida (ISO 8601).");
    } else if (Date.parse(timestamp) > Date.now() + MAX_CLOCK_AHEAD_MS) {
        errors.push("timestamp no puede estar mas de 5 minutos por delante del reloj del servidor.");
    }
    if (!isOptionalNumberInRange(battery_percent, 0, 100, true)) {
        errors.push("battery_percent es opcional; si se envia debe ser un entero entre 0 y 100 o null.");
    }
    if (!isOptionalNumberInRange(satellites, 0, 255, true)) {
        errors.push("satellites es opcional; si se envia debe ser un entero entre 0 y 255 o null.");
    }
    if (!isOptionalNumberInRange(packet_seq, 0, 65535, true)) {
        errors.push("packet_seq es opcional; si se envia debe ser un entero entre 0 y 65535 o null.");
    }
    if (!isOptionalNumberInRange(rssi, -200, 0, false)) {
        errors.push("rssi es opcional; si se envia debe ser un numero entre -200 y 0 (dBm) o null.");
    }
    if (!isOptionalNumberInRange(snr, -50, 50, false)) {
        errors.push("snr es opcional; si se envia debe ser un numero entre -50 y 50 (dB) o null.");
    }

    if (errors.length > 0) {
        return res.status(400).json({ status: "error", message: "Datos invalidos.", errors });
    }

    try {
        const animalResult = await pool.query(
            "SELECT id, is_active FROM animals WHERE collar_device_id = $1",
            [collar_device_id.trim()]
        );

        if (animalResult.rowCount === 0) {
            return res.status(404).json({
                status: "error",
                message: `No existe ningun animal registrado con collar_device_id "${collar_device_id}".`,
            });
        }

        const animalId = animalResult.rows[0].id;
        const animalIsActive: boolean = animalResult.rows[0].is_active;

        const geofenceResult = await pool.query("SELECT id FROM geofence WHERE is_active");
        if (geofenceResult.rowCount === 0) {
            return res.status(500).json({
                status: "error",
                message: "No hay ninguna geocerca activa en el sistema.",
            });
        }
        const geofenceId = geofenceResult.rows[0].id;

        // ST_Contains y el INSERT van en una sola sentencia para que inside_geofence corresponda
        // exactamente al poligono de geofence_id. ON CONFLICT hace la ingesta idempotente: una
        // lectura repetida (mismo animal y misma hora GPS) no se inserta.
        const insertResult = await pool.query(
            `WITH inserted AS (
                INSERT INTO locations (animal_id, geofence_id, geom, recorded_at, inside_geofence,
                                       battery_percent, satellites, packet_seq, rssi, snr)
                SELECT $1, g.id,
                       ST_SetSRID(ST_MakePoint($3, $4), 4326),
                       $5,
                       ST_Contains(g.geom, ST_SetSRID(ST_MakePoint($3, $4), 4326)),
                       $6, $7, $8, $9, $10
                FROM geofence g
                WHERE g.id = $2
                ON CONFLICT (animal_id, recorded_at) DO NOTHING
                RETURNING *
             )
             SELECT ${LOCATION_COLUMNS} FROM inserted`,
            [animalId, geofenceId, lon, lat, timestamp, battery_percent ?? null, satellites ?? null,
             packet_seq ?? null, rssi ?? null, snr ?? null]
        );

        if (insertResult.rowCount === 0) {
            // Ya registrada (p. ej. la estacion base reintento tras un timeout): no se reevalua
            // la geocerca ni se dispara otra alerta. 200 = exito para quien reintenta.
            const existing = await pool.query(
                `SELECT ${LOCATION_COLUMNS} FROM locations WHERE animal_id = $1 AND recorded_at = $2`,
                [animalId, timestamp]
            );
            return res.status(200).json({
                ...existing.rows[0],
                status: "duplicate",
                message: "Lectura ya registrada.",
                geofence_event: null,
            });
        }

        const location = insertResult.rows[0];

        // Animal inactivo: la lectura se guarda (trazabilidad) pero no genera eventos ni alertas.
        if (!animalIsActive) {
            return res.status(201).json({ ...location, geofence_event: null });
        }

        const geofenceEvent = await evaluateGeofenceTransition({
            animalId: location.animal_id,
            geofenceId: location.geofence_id,
            locationId: location.id,
            insideGeofence: location.inside_geofence,
            recordedAt: location.recorded_at,
        });

        res.status(201).json({ ...location, geofence_event: geofenceEvent });
    } catch (err) {
        sendInternalError(res, err);
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
            `SELECT ${LOCATION_COLUMNS}
             FROM locations
             WHERE animal_id = $1
             ORDER BY recorded_at DESC, id DESC
             LIMIT $2`,
            [animalId, limit]
        );

        res.json(locationsResult.rows);
    } catch (err) {
        sendInternalError(res, err);
    }
});
