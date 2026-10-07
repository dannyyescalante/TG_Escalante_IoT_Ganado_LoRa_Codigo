import fs from "fs";
import path from "path";
import { Client } from "pg";
import dotenv from "dotenv";

dotenv.config();

const targetDatabase = process.env.PGDATABASE as string;

const baseConfig = {
    host: process.env.PGHOST,
    port: Number(process.env.PGPORT),
    user: process.env.PGUSER,
    password: process.env.PGPASSWORD,
};

async function ensureDatabaseExists(): Promise<void> {
    // template1 existe siempre; la base "postgres" puede haber sido eliminada (paso en este servidor).
    const client = new Client({ ...baseConfig, database: "template1" });
    await client.connect();

    const result = await client.query(
        "SELECT 1 FROM pg_database WHERE datname = $1",
        [targetDatabase]
    );

    if (result.rowCount === 0) {
        await client.query(`CREATE DATABASE ${targetDatabase}`);
        console.log(`Base de datos "${targetDatabase}" creada.`);
    } else {
        console.log(`Base de datos "${targetDatabase}" ya existe.`);
    }

    await client.end();
}

async function applySchema(): Promise<void> {
    const client = new Client({ ...baseConfig, database: targetDatabase });
    await client.connect();

    const schemaPath = path.join(__dirname, "schema.sql");
    const schemaSql = fs.readFileSync(schemaPath, "utf-8");

    // Una sola transaccion: ante cualquier error (incluidos los RAISE de las migraciones)
    // PostgreSQL revierte tambien los cambios de estructura y la base queda como estaba.
    try {
        await client.query("BEGIN");
        await client.query(schemaSql);
        await client.query("COMMIT");
        console.log("Schema aplicado correctamente.");
    } catch (err) {
        await client.query("ROLLBACK");
        console.error("Se revirtieron todos los cambios de esta migracion.");
        throw err;
    } finally {
        await client.end();
    }
}

async function main(): Promise<void> {
    await ensureDatabaseExists();
    await applySchema();
}

main().catch((err) => {
    console.error("Error al migrar la base de datos:", err);
    process.exit(1);
});
