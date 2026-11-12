import { Router, Request, Response } from "express";
import { pool } from "../config/database";
import { sendInternalError } from "../http/errors";

export const animalsRouter = Router();

function parseAnimalId(rawId: string): number | null {
    if (!/^\d+$/.test(rawId)) return null;
    return Number(rawId);
}

function isNonEmptyString(value: unknown): value is string {
    return typeof value === "string" && value.trim().length > 0;
}

function isUniqueViolation(err: unknown): boolean {
    return typeof err === "object" && err !== null && (err as { code?: string }).code === "23505";
}

// GET /animals
animalsRouter.get("/", async (_req: Request, res: Response) => {
    try {
        const result = await pool.query("SELECT * FROM animals ORDER BY id");
        res.json(result.rows);
    } catch (err) {
        sendInternalError(res, err);
    }
});

// GET /animals/:id
animalsRouter.get("/:id", async (req: Request, res: Response) => {
    const id = parseAnimalId(req.params.id);
    if (id === null) {
        return res.status(400).json({ status: "error", message: "El id debe ser un numero entero positivo." });
    }

    try {
        const result = await pool.query("SELECT * FROM animals WHERE id = $1", [id]);
        if (result.rowCount === 0) {
            return res.status(404).json({ status: "error", message: `No existe un animal con id ${id}.` });
        }
        res.json(result.rows[0]);
    } catch (err) {
        sendInternalError(res, err);
    }
});

// POST /animals
animalsRouter.post("/", async (req: Request, res: Response) => {
    const { tag_number, name, breed, collar_device_id, collar_installed_at } = req.body ?? {};

    const errors: string[] = [];
    if (!isNonEmptyString(tag_number)) errors.push("tag_number es requerido y debe ser un texto no vacio.");
    if (!isNonEmptyString(collar_device_id)) errors.push("collar_device_id es requerido y debe ser un texto no vacio.");
    if (name !== undefined && name !== null && typeof name !== "string") errors.push("name debe ser un texto.");
    if (breed !== undefined && breed !== null && typeof breed !== "string") errors.push("breed debe ser un texto.");
    if (
        collar_installed_at !== undefined &&
        collar_installed_at !== null &&
        Number.isNaN(Date.parse(collar_installed_at))
    ) {
        errors.push("collar_installed_at debe ser una fecha valida (ISO 8601).");
    }

    if (errors.length > 0) {
        return res.status(400).json({ status: "error", message: "Datos invalidos.", errors });
    }

    try {
        const result = await pool.query(
            `INSERT INTO animals (tag_number, name, breed, collar_device_id, collar_installed_at)
             VALUES ($1, $2, $3, $4, $5)
             RETURNING *`,
            [tag_number.trim(), name ?? null, breed ?? null, collar_device_id.trim(), collar_installed_at ?? null]
        );
        res.status(201).json(result.rows[0]);
    } catch (err) {
        if (isUniqueViolation(err)) {
            return res.status(409).json({
                status: "error",
                message: "Ya existe un animal con ese tag_number o collar_device_id.",
            });
        }
        sendInternalError(res, err);
    }
});

// PUT /animals/:id
animalsRouter.put("/:id", async (req: Request, res: Response) => {
    const id = parseAnimalId(req.params.id);
    if (id === null) {
        return res.status(400).json({ status: "error", message: "El id debe ser un numero entero positivo." });
    }

    const { tag_number, name, breed, collar_device_id, collar_installed_at, is_active } = req.body ?? {};

    const errors: string[] = [];
    if (tag_number !== undefined && !isNonEmptyString(tag_number)) errors.push("tag_number no puede estar vacio.");
    if (collar_device_id !== undefined && !isNonEmptyString(collar_device_id)) {
        errors.push("collar_device_id no puede estar vacio.");
    }
    if (name !== undefined && name !== null && typeof name !== "string") errors.push("name debe ser un texto.");
    if (breed !== undefined && breed !== null && typeof breed !== "string") errors.push("breed debe ser un texto.");
    if (
        collar_installed_at !== undefined &&
        collar_installed_at !== null &&
        Number.isNaN(Date.parse(collar_installed_at))
    ) {
        errors.push("collar_installed_at debe ser una fecha valida (ISO 8601).");
    }
    if (is_active !== undefined && typeof is_active !== "boolean") errors.push("is_active debe ser booleano.");

    const fields: Record<string, unknown> = {
        tag_number,
        name,
        breed,
        collar_device_id,
        collar_installed_at,
        is_active,
    };

    const setClauses: string[] = [];
    const values: unknown[] = [];

    for (const [column, rawValue] of Object.entries(fields)) {
        if (rawValue === undefined) continue;
        const value = typeof rawValue === "string" ? rawValue.trim() : rawValue;
        values.push(value);
        setClauses.push(`${column} = $${values.length}`);
    }

    if (setClauses.length === 0) {
        errors.push("Debe enviar al menos un campo para actualizar.");
    }

    if (errors.length > 0) {
        return res.status(400).json({ status: "error", message: "Datos invalidos.", errors });
    }

    setClauses.push("updated_at = now()");
    values.push(id);

    try {
        const result = await pool.query(
            `UPDATE animals SET ${setClauses.join(", ")} WHERE id = $${values.length} RETURNING *`,
            values
        );
        if (result.rowCount === 0) {
            return res.status(404).json({ status: "error", message: `No existe un animal con id ${id}.` });
        }
        res.json(result.rows[0]);
    } catch (err) {
        if (isUniqueViolation(err)) {
            return res.status(409).json({
                status: "error",
                message: "Ya existe un animal con ese tag_number o collar_device_id.",
            });
        }
        sendInternalError(res, err);
    }
});
