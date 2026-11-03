import { useEffect, useRef } from 'react'
import { geoJSON, latLng, latLngBounds, type LatLngBounds, type LatLngTuple } from 'leaflet'
import { GeoJSON, LayersControl, MapContainer, Marker, Popup, TileLayer, Tooltip, useMap } from 'react-leaflet'
import { BASE_LAYERS, FIT_MAX_DISTANCE_M, MAP_MAX_ZOOM, STATUS_COLORS } from '../config'
import { formatDateTime, formatTimeAgo } from '../format'
import { isConfirmedOutside, type HerdPosition } from '../hooks/useHerdPositions'
import type { Geofence } from '../types'
import { cowIcon } from './cowIcon'

// Vista inicial mientras llegan los datos (San Cristobal, Tachira).
const FALLBACK_CENTER: LatLngTuple = [7.7669, -72.225]
const FALLBACK_ZOOM = 12

interface HerdMapProps {
  geofence: Geofence | null
  geofenceFailed: boolean
  positions: HerdPosition[]
  positionsLoaded: boolean
  // Referencia para "hace X min": la hora de la ultima consulta (cambia cada 30 s, sin otro temporizador).
  lastUpdated?: Date | null
}

function toPoint(position: HerdPosition): LatLngTuple | null {
  return position.location ? [position.location.lat, position.location.lon] : null
}

// Distancia en metros del punto al rectangulo que envuelve la geocerca (0 si esta adentro).
// Es una aproximacion suficiente para decidir si el animal "esta cerca" del potrero.
function distanceToBounds(bounds: LatLngBounds, point: LatLngTuple): number {
  const [lat, lon] = point
  const nearestLat = Math.min(Math.max(lat, bounds.getSouth()), bounds.getNorth())
  const nearestLon = Math.min(Math.max(lon, bounds.getWest()), bounds.getEast())
  return latLng(lat, lon).distanceTo(latLng(nearestLat, nearestLon))
}

// Encuadra una sola vez, cuando ya llegaron la geocerca y la primera consulta de posiciones:
// si se reencuadrara en cada consulta, el zoom del usuario se perderia cada 30 s.
// Incluye la geocerca y solo los animales a menos de FIT_MAX_DISTANCE_M de ella; si no se pudo
// cargar la geocerca, encuadra a todos los animales.
function FitOnce({ geofence, geofenceFailed, positions, positionsLoaded }: HerdMapProps) {
  const map = useMap()
  const fitted = useRef(false)

  useEffect(() => {
    if (fitted.current || !positionsLoaded) return

    const points = positions.map(toPoint).filter((p): p is LatLngTuple => p !== null)
    let bounds: LatLngBounds | null = null
    if (geofence) {
      const fenceBounds = geoJSON(geofence.geometry).getBounds()
      const nearby = points.filter((p) => distanceToBounds(fenceBounds, p) <= FIT_MAX_DISTANCE_M)
      bounds = latLngBounds(fenceBounds.getSouthWest(), fenceBounds.getNorthEast())
      nearby.forEach((p) => bounds!.extend(p))
    } else if (geofenceFailed && points.length > 0) {
      bounds = latLngBounds(points)
    }

    if (bounds && bounds.isValid()) {
      map.fitBounds(bounds, { padding: [48, 48], maxZoom: 18 })
      fitted.current = true
    }
  }, [map, geofence, geofenceFailed, positions, positionsLoaded])

  return null
}

export function HerdMap({ geofence, geofenceFailed, positions, positionsLoaded, lastUpdated }: HerdMapProps) {
  // Mismo criterio que la leyenda y las alertas: salida confirmada (ultimo evento), no la ultima lectura.
  const anyOutside = positions.some((p) => p.location !== null && isConfirmedOutside(p))
  const fenceColor = anyOutside ? STATUS_COLORS.outside : STATUS_COLORS.inside

  return (
    <MapContainer center={FALLBACK_CENTER} zoom={FALLBACK_ZOOM} maxZoom={MAP_MAX_ZOOM} className="herd-map">
      <LayersControl position="topleft">
        <LayersControl.BaseLayer checked name={BASE_LAYERS.satellite.name}>
          <TileLayer
            url={BASE_LAYERS.satellite.url}
            attribution={BASE_LAYERS.satellite.attribution}
            maxNativeZoom={BASE_LAYERS.satellite.maxNativeZoom}
            maxZoom={MAP_MAX_ZOOM}
          />
        </LayersControl.BaseLayer>
        <LayersControl.BaseLayer name={BASE_LAYERS.streets.name}>
          <TileLayer
            url={BASE_LAYERS.streets.url}
            attribution={BASE_LAYERS.streets.attribution}
            maxNativeZoom={BASE_LAYERS.streets.maxNativeZoom}
            maxZoom={MAP_MAX_ZOOM}
          />
        </LayersControl.BaseLayer>
      </LayersControl>

      {geofence && (
        // El estilo de GeoJSON no se actualiza solo: la key cambia con el estado para redibujarlo.
        <GeoJSON
          key={`${geofence.id}-${anyOutside ? 'fuera' : 'dentro'}`}
          data={geofence.geometry}
          style={{ color: fenceColor, weight: 3, fillColor: fenceColor, fillOpacity: 0.22 }}
        />
      )}

      {positions.map((position) => {
        const { animal, location, lastEvent } = position
        if (!location) return null
        const outside = isConfirmedOutside(position)

        return (
          <Marker
            key={animal.id}
            position={[location.lat, location.lon]}
            icon={cowIcon(outside)}
            title={animal.tag_number}
            alt={animal.tag_number}
          >
            <Tooltip permanent direction="bottom" className="cow-label">
              {animal.tag_number}
            </Tooltip>
            <Popup>
              <div className="popup">
                <p className="popup-title">
                  <strong>{animal.tag_number}</strong>
                  {animal.name && <span> · {animal.name}</span>}
                </p>
                <p className={outside ? 'popup-status popup-status--out' : 'popup-status popup-status--in'}>
                  {outside && lastEvent
                    ? `Fuera de la geocerca desde ${formatDateTime(lastEvent.occurred_at)}`
                    : 'Dentro de la geocerca'}
                </p>
                <dl className="popup-data">
                  <dt>Batería</dt>
                  <dd>{location.battery_percent !== null ? `${location.battery_percent} %` : 'sin dato'}</dd>
                  <dt>Última lectura</dt>
                  <dd>
                    {lastUpdated ? formatTimeAgo(location.recorded_at, lastUpdated) : '—'}
                    <br />
                    <small>{formatDateTime(location.recorded_at)}</small>
                  </dd>
                  <dt>Posición</dt>
                  <dd>
                    {location.lat.toFixed(6)}, {location.lon.toFixed(6)}
                  </dd>
                </dl>
              </div>
            </Popup>
          </Marker>
        )
      })}

      <FitOnce
        geofence={geofence}
        geofenceFailed={geofenceFailed}
        positions={positions}
        positionsLoaded={positionsLoaded}
      />
    </MapContainer>
  )
}
