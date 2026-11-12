import express, { Express, NextFunction, Request, Response } from "express";
import { pool } from "./config/database";
import { sendInternalError } from "./http/errors";
import { animalsRouter } from "./routes/animals.routes";
import { locationsRouter } from "./routes/locations.routes";
import { eventsRouter } from "./routes/events.routes";
import { geofenceRouter } from "./routes/geofence.routes";

export const app: Express = express();

app.use(express.json());

app.use("/animals", animalsRouter);
app.use("/locations", locationsRouter);
app.use("/events", eventsRouter);
app.use("/geofence", geofenceRouter);

app.get("/health", async (_req: Request, res: Response) => {
    try {
        const result = await pool.query("SELECT PostGIS_Version()");
        res.json({
            status: "ok",
            database: "connected",
            postgis: result.rows[0].postgis_version,
        });
    } catch (err) {
        sendInternalError(res, err);
    }
});

// Va al final: recibe los errores de express.json() (JSON mal formado) y cualquier error no
// capturado en una ruta. Responde JSON sin traza ni rutas del disco (el manejador por defecto
// de Express devuelve HTML con la traza).
app.use((err: unknown, _req: Request, res: Response, _next: NextFunction) => {
    const parserError = err as { type?: string; status?: number };
    if (parserError.type === "entity.parse.failed") {
        return res.status(400).json({ status: "error", message: "JSON mal formado." });
    }
    if (parserError.status !== undefined && parserError.status >= 400 && parserError.status < 500) {
        return res.status(parserError.status).json({ status: "error", message: "Solicitud invalida." });
    }
    sendInternalError(res, err);
});
