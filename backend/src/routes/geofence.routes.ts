import { Router, Request, Response } from "express";
import { pool } from "../config/database";
import { sendInternalError } from "../http/errors";

export const geofenceRouter = Router();

// GET /geofence
geofenceRouter.get("/", async (_req: Request, res: Response) => {
    try {
        // La misma geocerca activa que usa POST /locations para calcular inside_geofence,
        // asi el mapa dibuja exactamente el poligono contra el que se evaluan las ubicaciones.
        const result = await pool.query(
            `SELECT id, name, ST_AsGeoJSON(geom)::json AS geometry
             FROM geofence
             WHERE is_active`
        );

        if (result.rowCount === 0) {
            return res.status(404).json({
                status: "error",
                message: "No hay ninguna geocerca activa en el sistema.",
            });
        }

        res.json(result.rows[0]);
    } catch (err) {
        sendInternalError(res, err);
    }
});
