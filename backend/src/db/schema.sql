-- ============================================================================
-- Esquema de la base de datos (PostgreSQL 17 + PostGIS).
-- "npm run migrate" lo ejecuta completo dentro de UNA transaccion: si algo falla,
-- no queda ningun cambio a medias. Se puede volver a ejecutar sin efectos.
--   * Los CREATE TABLE tienen la forma final (instalacion desde cero).
--   * Los bloques "Migracion" llevan una base ya creada a esa misma forma;
--     nunca borran datos por su cuenta: ante una inconsistencia abortan.
-- ============================================================================

CREATE EXTENSION IF NOT EXISTS postgis;

-- ----------------------------------------------------------------------------
-- animals
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS animals (
    id SERIAL PRIMARY KEY,
    tag_number VARCHAR(50) NOT NULL UNIQUE,
    name VARCHAR(100),
    breed VARCHAR(100),
    collar_device_id VARCHAR(100) NOT NULL UNIQUE,
    collar_installed_at TIMESTAMPTZ,
    is_active BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON COLUMN animals.collar_device_id IS
    'Decision de diseno: durante la validacion cada uno de los 5 collares queda fijo a su animal, '
    'por eso el dispositivo no se modela en una tabla aparte.';

-- ----------------------------------------------------------------------------
-- geofence: una sola activa; las anteriores se conservan como historial para que
-- locations y geofence_events sigan apuntando al poligono exacto que se uso.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS geofence (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    geom GEOMETRY(POLYGON, 4326) NOT NULL,
    is_active BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Migracion: is_active. Antes el backend usaba la geocerca de menor id; esa queda activa.
ALTER TABLE geofence ADD COLUMN IF NOT EXISTS is_active BOOLEAN NOT NULL DEFAULT true;
UPDATE geofence SET is_active = false
WHERE is_active AND id <> (SELECT min(id) FROM geofence WHERE is_active);

CREATE UNIQUE INDEX IF NOT EXISTS uq_geofence_single_active ON geofence (is_active) WHERE is_active;
CREATE INDEX IF NOT EXISTS idx_geofence_geom ON geofence USING GIST(geom);

-- ----------------------------------------------------------------------------
-- locations
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS locations (
    id BIGSERIAL PRIMARY KEY,
    animal_id INTEGER NOT NULL REFERENCES animals(id) ON DELETE CASCADE,
    geofence_id INTEGER NOT NULL REFERENCES geofence(id),
    geom GEOMETRY(POINT, 4326) NOT NULL,
    recorded_at TIMESTAMPTZ NOT NULL,
    received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    inside_geofence BOOLEAN NOT NULL,
    battery_percent SMALLINT CHECK (battery_percent BETWEEN 0 AND 100),
    satellites SMALLINT CHECK (satellites BETWEEN 0 AND 255),
    packet_seq INTEGER CHECK (packet_seq BETWEEN 0 AND 65535),
    rssi REAL,
    snr REAL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Migracion: columnas nuevas (NULL en las filas existentes).
ALTER TABLE locations ADD COLUMN IF NOT EXISTS battery_percent SMALLINT
    CHECK (battery_percent BETWEEN 0 AND 100);
ALTER TABLE locations ADD COLUMN IF NOT EXISTS geofence_id INTEGER REFERENCES geofence(id);
ALTER TABLE locations ADD COLUMN IF NOT EXISTS satellites SMALLINT CHECK (satellites BETWEEN 0 AND 255);
ALTER TABLE locations ADD COLUMN IF NOT EXISTS packet_seq INTEGER CHECK (packet_seq BETWEEN 0 AND 65535);
ALTER TABLE locations ADD COLUMN IF NOT EXISTS rssi REAL;
ALTER TABLE locations ADD COLUMN IF NOT EXISTS snr REAL;

-- Migracion: geofence_id de las lecturas existentes. Aproximacion: hasta esta migracion el
-- seed sobrescribia el poligono de la geocerca, asi que no se puede saber cual se uso; se
-- asigna la activa.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations WHERE geofence_id IS NULL)
       AND NOT EXISTS (SELECT 1 FROM geofence WHERE is_active) THEN
        RAISE EXCEPTION 'Migracion abortada: hay lecturas sin geofence_id y no existe una geocerca activa para asignarles.';
    END IF;
END $$;
UPDATE locations SET geofence_id = (SELECT id FROM geofence WHERE is_active)
WHERE geofence_id IS NULL;
ALTER TABLE locations ALTER COLUMN geofence_id SET NOT NULL;

-- Migracion: idempotencia. Si ya hay lecturas repetidas se aborta en vez de borrar alguna.
DO $$
DECLARE
    duplicated INTEGER;
BEGIN
    SELECT count(*) INTO duplicated
    FROM (SELECT 1 FROM locations GROUP BY animal_id, recorded_at HAVING count(*) > 1) d;
    IF duplicated > 0 THEN
        RAISE EXCEPTION 'Migracion abortada: % combinaciones (animal_id, recorded_at) repetidas en locations. Revisarlas a mano; no se borra nada automaticamente.', duplicated;
    END IF;
END $$;

CREATE UNIQUE INDEX IF NOT EXISTS uq_locations_animal_recorded ON locations (animal_id, recorded_at);
CREATE INDEX IF NOT EXISTS idx_locations_animal_id ON locations(animal_id);
CREATE INDEX IF NOT EXISTS idx_locations_recorded_at ON locations(recorded_at);
CREATE INDEX IF NOT EXISTS idx_locations_geom ON locations USING GIST(geom);

-- ----------------------------------------------------------------------------
-- geofence_events: el animal se obtiene por locations.animal_id (sin redundancia).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS geofence_events (
    id BIGSERIAL PRIMARY KEY,
    location_id BIGINT NOT NULL REFERENCES locations(id) ON DELETE CASCADE,
    geofence_id INTEGER NOT NULL REFERENCES geofence(id),
    event_type VARCHAR(10) NOT NULL CHECK (event_type IN ('exit', 'enter')),
    occurred_at TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Migracion: geofence_id tomado de la lectura que confirmo cada evento.
ALTER TABLE geofence_events ADD COLUMN IF NOT EXISTS geofence_id INTEGER REFERENCES geofence(id);
UPDATE geofence_events ge SET geofence_id = l.geofence_id
FROM locations l
WHERE l.id = ge.location_id AND ge.geofence_id IS NULL;
ALTER TABLE geofence_events ALTER COLUMN geofence_id SET NOT NULL;

-- Migracion: quitar animal_id, solo si coincide en todos los eventos con el de su lectura.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns
               WHERE table_name = 'geofence_events' AND column_name = 'animal_id') THEN
        IF EXISTS (SELECT 1 FROM geofence_events ge JOIN locations l ON l.id = ge.location_id
                   WHERE ge.animal_id <> l.animal_id) THEN
            RAISE EXCEPTION 'Migracion abortada: hay eventos cuyo animal_id no coincide con el animal de su lectura.';
        END IF;
        ALTER TABLE geofence_events DROP COLUMN animal_id;
    END IF;
END $$;

CREATE INDEX IF NOT EXISTS idx_geofence_events_location_id ON geofence_events(location_id);
CREATE INDEX IF NOT EXISTS idx_geofence_events_geofence_id ON geofence_events(geofence_id);
CREATE INDEX IF NOT EXISTS idx_geofence_events_occurred_at ON geofence_events(occurred_at);

-- ----------------------------------------------------------------------------
-- alerts: un registro por cada intento de notificacion (mide RNF1 con
-- alerts.sent_at - geofence_events.created_at).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS alerts (
    id BIGSERIAL PRIMARY KEY,
    geofence_event_id BIGINT NOT NULL REFERENCES geofence_events(id) ON DELETE CASCADE,
    channel VARCHAR(20) NOT NULL CHECK (channel IN ('telegram')),
    status VARCHAR(10) NOT NULL CHECK (status IN ('sent', 'failed')),
    response_code INTEGER,
    telegram_message_id BIGINT,
    error_detail VARCHAR(100),
    sent_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_alerts_geofence_event_id ON alerts(geofence_event_id);
