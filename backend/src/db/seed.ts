import { Client } from "pg";
import dotenv from "dotenv";

dotenv.config();

/**
 * ============================================================================
 * PLACEHOLDER TEMPORAL — NO SON COORDENADAS REALES
 * ============================================================================
 * Este polígono es un rectángulo sintético de ~2 hectáreas (200m x 100m)
 * ubicado como referencia general cerca del municipio García de Hevia,
 * estado Táchira, Venezuela (~8.22°N, -72.27°O).
 *
 * DEBE reemplazarse por las coordenadas reales del corral piloto una vez
 * se capturen en campo (ej. con GPS/RTK sobre el terreno real). No usar
 * este polígono para ninguna decisión operativa o de validación real.
 * ============================================================================
 */
const PLACEHOLDER_GEOFENCE_NAME = "Potrero piloto (PLACEHOLDER - reemplazar con coordenadas reales)";
const PLACEHOLDER_GEOFENCE_WKT =
    "POLYGON((" +
    "-72.270908 8.219551, " +
    "-72.269092 8.219551, " +
    "-72.269092 8.220449, " +
    "-72.270908 8.220449, " +
    "-72.270908 8.219551" +
    "))";

async function seedGeofence(): Promise<void> {
    const client = new Client({
        host: process.env.PGHOST,
        port: Number(process.env.PGPORT),
        user: process.env.PGUSER,
        password: process.env.PGPASSWORD,
        database: process.env.PGDATABASE,
    });

    await client.connect();

    const { rows: existing } = await client.query(
        "SELECT id FROM geofence WHERE name = $1",
        [PLACEHOLDER_GEOFENCE_NAME]
    );

    if (existing.length > 0) {
        console.log("El polígono placeholder ya existe (id " + existing[0].id + "), no se vuelve a insertar.");
    } else {
        const insertResult = await client.query(
            "INSERT INTO geofence (name, geom) VALUES ($1, ST_GeomFromText($2, 4326)) RETURNING id",
            [PLACEHOLDER_GEOFENCE_NAME, PLACEHOLDER_GEOFENCE_WKT]
        );
        console.log("Polígono placeholder insertado con id " + insertResult.rows[0].id);
    }

    const verify = await client.query(
        `SELECT
            id,
            name,
            ST_AsText(geom) AS wkt,
            ST_Area(geom::geography) AS area_m2,
            ST_Area(geom::geography) / 10000 AS area_hectareas
         FROM geofence
         WHERE name = $1`,
        [PLACEHOLDER_GEOFENCE_NAME]
    );

    console.log("\n--- Verificación PostGIS ---");
    console.table(
        verify.rows.map((r) => ({
            id: r.id,
            name: r.name,
            area_m2: Number(r.area_m2).toFixed(2),
            area_hectareas: Number(r.area_hectareas).toFixed(4),
        }))
    );
    console.log("WKT:", verify.rows[0].wkt);

    await client.end();
}

seedGeofence().catch((err) => {
    console.error("Error al sembrar la geocerca placeholder:", err);
    process.exit(1);
});
