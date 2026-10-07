import { useEffect, useRef } from 'react'
import { geoJSON, latLngBounds, type LatLngBounds, type LatLngTuple } from 'leaflet'
import { CircleMarker, GeoJSON, MapContainer, Popup, TileLayer, useMap } from 'react-leaflet'
import { STATUS_COLORS } from '../config'
import { formatDateTime } from '../format'
import { isConfirmedOutside, type HerdPosition } from '../hooks/useHerdPositions'
import type { Geofence } from '../types'

// Vista inicial mientras llegan los datos (San Cristobal, Tachira).
const FALLBACK_CENTER: LatLngTuple = [7.7669, -72.225]
const FALLBACK_ZOOM = 12

interface HerdMapProps {
  geofence: Geofence | null
  geofenceFailed: boolean
  positions: HerdPosition[]
}

// Encuadra una sola vez: si se reencuadrara en cada consulta, el zoom del usuario se perderia cada 30 s.
// Prioriza el poligono; solo si no se pudo cargar, encuadra a los animales.
function FitOnce({ geofence, geofenceFailed, positions }: HerdMapProps) {
  const map = useMap()
  const fitted = useRef(false)

  useEffect(() => {
    if (fitted.current) return

    let bounds: LatLngBounds | null = null
    if (geofence) {
      bounds = geoJSON(geofence.geometry).getBounds()
    } else if (geofenceFailed) {
      const points = positions.flatMap((p) => (p.location ? [[p.location.lat, p.location.lon] as LatLngTuple] : []))
      if (points.length > 0) bounds = latLngBounds(points)
    }

    if (bounds && bounds.isValid()) {
      map.fitBounds(bounds, { padding: [40, 40], maxZoom: 18 })
      fitted.current = true
    }
  }, [map, geofence, geofenceFailed, positions])

  return null
}

export function HerdMap({ geofence, geofenceFailed, positions }: HerdMapProps) {
  return (
    <MapContainer center={FALLBACK_CENTER} zoom={FALLBACK_ZOOM} className="herd-map">
      <TileLayer
        attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>'
        url="https://tile.openstreetmap.org/{z}/{x}/{y}.png"
      />

      {geofence && (
        <GeoJSON
          key={geofence.id}
          data={geofence.geometry}
          style={{ color: STATUS_COLORS.geofence, weight: 2, fillOpacity: 0.08 }}
        />
      )}

      {positions.map((position) => {
        const { animal, location, lastEvent } = position
        if (!location) return null
        const outside = isConfirmedOutside(position)

        return (
          <CircleMarker
            key={animal.id}
            center={[location.lat, location.lon]}
            radius={9}
            pathOptions={{
              color: '#ffffff',
              weight: 2,
              fillColor: outside ? STATUS_COLORS.outside : STATUS_COLORS.inside,
              fillOpacity: 0.95,
            }}
          >
            <Popup>
              <strong>{animal.tag_number}</strong>
              {animal.name && ` · ${animal.name}`}
              <br />
              {outside && lastEvent
                ? `Fuera de la geocerca (salida confirmada: ${formatDateTime(lastEvent.occurred_at)})`
                : 'Dentro de la geocerca'}
              <br />
              Última lectura: {formatDateTime(location.recorded_at)}
              <br />
              {location.lat.toFixed(6)}, {location.lon.toFixed(6)}
            </Popup>
          </CircleMarker>
        )
      })}

      <FitOnce geofence={geofence} geofenceFailed={geofenceFailed} positions={positions} />
    </MapContainer>
  )
}
