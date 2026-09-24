import { Router, Request, Response } from "express";
import { pool } from "../config/database";

export const eventsRouter = Router();

function parsePositiveInt(raw: string): number | null {
    if (!/^\d+$/.test(raw)) return null;
    return Number(raw);
}

function firstQueryValue(value: unknown): string | undefined {
    if (Array.isArray(value)) return typeof value[0] === "string" ? value[0] : undefined;
    return typeof value === "string" ? value : undefined;
}

// GET /events
eventsRouter.get("/", async (req: Request, res: Response) => {
    const errors: string[] = [];

    let animalId: number | undefined;
    const rawAnimalId = firstQueryValue(req.query.animal_id);
    if (rawAnimalId !== undefined) {
        const parsed = parsePositiveInt(rawAnimalId);
        if (parsed === null) {
            errors.push("animal_id debe ser un numero entero positivo.");
        } else {
            animalId = parsed;
        }
    }

    const rawFrom = firstQueryValue(req.query.from);
    if (rawFrom !== undefined && Number.isNaN(Date.parse(rawFrom))) {
        errors.push("from debe ser una fecha valida (ISO 8601).");
    }

    const rawTo = firstQueryValue(req.query.to);
    if (rawTo !== undefined && Number.isNaN(Date.parse(rawTo))) {
        errors.push("to debe ser una fecha valida (ISO 8601).");
    }

    if (errors.length > 0) {
        return res.status(400).json({ status: "error", message: "Parametros invalidos.", errors });
    }

    const conditions: string[] = [];
    const values: unknown[] = [];

    if (animalId !== undefined) {
        values.push(animalId);
        conditions.push(`ge.animal_id = $${values.length}`);
    }
    if (rawFrom !== undefined) {
        values.push(rawFrom);
        conditions.push(`ge.occurred_at >= $${values.length}`);
    }
    if (rawTo !== undefined) {
        values.push(rawTo);
        conditions.push(`ge.occurred_at <= $${values.length}`);
    }

    const whereClause = conditions.length > 0 ? `WHERE ${conditions.join(" AND ")}` : "";

    try {
        const result = await pool.query(
            `SELECT ge.id, ge.animal_id, a.tag_number, a.name AS animal_name,
                    ge.location_id, ge.event_type, ge.occurred_at, ge.created_at
             FROM geofence_events ge
             JOIN animals a ON a.id = ge.animal_id
             ${whereClause}
             ORDER BY ge.occurred_at DESC, ge.id DESC`,
            values
        );
        res.json(result.rows);
    } catch (err) {
        res.status(500).json({ status: "error", message: (err as Error).message });
    }
});
