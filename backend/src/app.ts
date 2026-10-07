import express, { Express, Request, Response } from "express";
import { pool } from "./config/database";
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
        res.status(500).json({ status: "error", message: (err as Error).message });
    }
});
