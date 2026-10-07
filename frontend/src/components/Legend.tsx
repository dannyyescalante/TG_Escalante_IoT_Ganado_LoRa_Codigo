import { POLL_INTERVAL_MS, STATUS_COLORS } from '../config'
import { formatTime } from '../format'
import { isConfirmedOutside, type HerdPosition } from '../hooks/useHerdPositions'
import type { Geofence } from '../types'

interface LegendProps {
  positions: HerdPosition[]
  lastUpdated: Date | null
  error: string | null
  loading: boolean
  geofence: Geofence | null
  geofenceError: string | null
}

export function Legend({ positions, lastUpdated, error, loading, geofence, geofenceError }: LegendProps) {
  const located = positions.filter((p) => p.location !== null)
  const outsideCount = located.filter(isConfirmedOutside).length
  const insideCount = located.length - outsideCount
  const withoutLocation = positions.filter((p) => p.location === null)

  return (
    <aside className="legend">
      <h2>Estado del hato</h2>

      <ul className="legend-status">
        <li>
          <span className="swatch" style={{ background: STATUS_COLORS.inside }} />
          Dentro de la geocerca: <strong>{insideCount}</strong>
        </li>
        <li>
          <span className="swatch" style={{ background: STATUS_COLORS.outside }} />
          Fuera (salida confirmada): <strong>{outsideCount}</strong>
        </li>
      </ul>

      {withoutLocation.length > 0 && (
        <p className="legend-note">
          Sin ubicaciones todavía: {withoutLocation.map((p) => p.animal.tag_number).join(', ')}
        </p>
      )}

      <p className="legend-note">
        Geocerca: {geofence ? geofence.name : geofenceError ? `no disponible (${geofenceError})` : 'cargando…'}
      </p>

      <p className={error ? 'legend-update legend-error' : 'legend-update'}>
        {loading && 'Cargando datos…'}
        {!loading && error && `No se pudo actualizar: ${error}.`}
        {!loading && lastUpdated && ` Última actualización: ${formatTime(lastUpdated)}`}
        {!loading && !error && ` (cada ${POLL_INTERVAL_MS / 1000} s)`}
      </p>
    </aside>
  )
}
