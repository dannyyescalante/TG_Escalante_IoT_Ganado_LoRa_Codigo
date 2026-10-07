import { Client } from "pg";
import dotenv from "dotenv";

dotenv.config();

/**
 * ============================================================================
 * GEOCERCA TEMPORAL DE PRUEBA — NO ES LA GEOCERCA REAL DE LA FINCA
 * ============================================================================
 * Cuadrado de 40 m x 40 m en San Cristobal, Tachira, usado para la prueba de
 * campo caminando con el collar en "Casa de Mafer" (centro 7.761292, -72.223944).
 * Se amplio a 40 m por lado porque el ruido del GPS NEO-6M (~7 m medido en campo)
 * generaria salidas falsas en un area mas pequena.
 *
 * Otra geocerca de prueba usada: cuadrilatero de ~1720 m2 en "Casa Danny"
 * (centro ~7.762124, -72.217737):
 *   POLYGON((-72.217501 7.762129, -72.217624 7.761883, -72.217980 7.762036,
 *            -72.217808 7.762436, -72.217501 7.762129))
 *
 * DEBE reemplazarse por el poligono real del corral piloto en Garcia de Hevia
 * cuando se capturen sus coordenadas en campo.
 * ============================================================================
 */
const GEOFENCE_NAME = "Prueba de campo - Casa de Mafer (TEMPORAL)";
const GEOFENCE_WKT =
    "POLYGON((" +
    "-72.224126 7.761111, " +
    "-72.224126 7.761473, " +
    "-72.223763 7.761473, " +
    "-72.223763 7.761111, " +
    "-72.224126 7.761111" +
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

    // Una sola geocerca activa (indice unico parcial). La anterior no se sobrescribe: queda
    // inactiva como historial, porque locations y geofence_events apuntan al poligono exacto usado.
    const { rows: active } = await client.query(
        "SELECT id, name, ST_Equals(geom, ST_GeomFromText($1, 4326)) AS same_geom FROM geofence WHERE is_active",
        [GEOFENCE_WKT]
    );

    let geofenceId: number;
    if (active.length > 0 && active[0].name === GEOFENCE_NAME && active[0].same_geom) {
        geofenceId = active[0].id;
        console.log("La geocerca activa (id " + geofenceId + ") ya es esta; no hay cambios.");
    } else {
        await client.query("BEGIN");
        try {
            await client.query("UPDATE geofence SET is_active = false WHERE is_active");
            const insertResult = await client.query(
                "INSERT INTO geofence (name, geom) VALUES ($1, ST_GeomFromText($2, 4326)) RETURNING id",
                [GEOFENCE_NAME, GEOFENCE_WKT]
            );
            await client.query("COMMIT");
            geofenceId = insertResult.rows[0].id;
        } catch (err) {
            await client.query("ROLLBACK");
            throw err;
        }
        const previous = active.length > 0 ? " (la anterior, id " + active[0].id + ", queda inactiva)" : "";
        console.log("Geocerca activa: id " + geofenceId + " - " + GEOFENCE_NAME + previous);
    }

    const verify = await client.query(
        `SELECT
            id,
            name,
            ST_IsValid(geom) AS valid,
            ST_AsText(geom) AS wkt,
            ST_Area(geom::geography) AS area_m2
         FROM geofence
         WHERE id = $1`,
        [geofenceId]
    );

    console.log("\n--- Verificacion PostGIS ---");
    console.table(
        verify.rows.map((r) => ({
            id: r.id,
            name: r.name,
            valida: r.valid,
            area_m2: Number(r.area_m2).toFixed(1),
        }))
    );
    console.log("WKT:", verify.rows[0].wkt);

    await client.end();
}

seedGeofence().catch((err) => {
    console.error("Error al sembrar la geocerca:", err);
    process.exit(1);
});
