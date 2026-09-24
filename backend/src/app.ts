import express, { Express, Request, Response } from "express";
import { pool } from "./config/database";

export const app: Express = express();

app.use(express.json());

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
