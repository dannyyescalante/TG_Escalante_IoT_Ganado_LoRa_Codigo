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
    const client = new Client({ ...baseConfig, database: "postgres" });
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

    await client.query(schemaSql);
    console.log("Schema aplicado correctamente.");

    await client.end();
}

async function main(): Promise<void> {
    await ensureDatabaseExists();
    await applySchema();
}

main().catch((err) => {
    console.error("Error al migrar la base de datos:", err);
    process.exit(1);
});
