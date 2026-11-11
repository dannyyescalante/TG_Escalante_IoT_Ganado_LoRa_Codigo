import { Response } from "express";

// El detalle del error solo va al log: al cliente no se le exponen mensajes de la BD,
// trazas ni rutas del disco (algunos errores de conexion de pg ademas traen message vacio).
export function sendInternalError(res: Response, err: unknown): void {
    console.error("Error interno del servidor:", err);
    res.status(500).json({ status: "error", message: "Error interno del servidor" });
}
