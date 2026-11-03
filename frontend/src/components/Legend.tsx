import { STATUS_COLORS } from '../config'
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
    <aside className="legend" aria-live="polite">
      <h2 className="legend-title">Estado del hato</h2>

      <div className="legend-counts">
        <div className="count" style={{ borderColor: STATUS_COLORS.inside }}>
          <span className="count-value" style={{ color: STATUS_COLORS.inside }}>
            {insideCount}
          </span>
          <span className="count-label">Dentro</span>
        </div>
        <div className="count" style={{ borderColor: STATUS_COLORS.outside }}>
          <span className="count-value" style={{ color: STATUS_COLORS.outside }}>
            {outsideCount}
          </span>
          <span className="count-label">Fuera</span>
          <span className="count-hint">salida confirmada</span>
        </div>
      </div>

      <dl className="legend-info">
        <dt>Geocerca</dt>
        <dd>{geofence ? geofence.name : geofenceError ? 'no disponible por ahora' : 'cargando…'}</dd>
        <dt>Última actualización</dt>
        <dd>
          <span className={error ? 'status-dot status-dot--error' : 'status-dot status-dot--ok'} />
          {lastUpdated ? formatTime(lastUpdated) : '—'}
        </dd>
      </dl>

      {loading && <p className="legend-message">Cargando datos…</p>}

      {!loading && error && (
        <p className="legend-message legend-message--error">
          Sin conexión con el servidor. Mostrando los últimos datos; se reintenta automáticamente.
        </p>
      )}

      {!loading && !error && located.length === 0 && (
        <p className="legend-message">
          Aún no hay ubicaciones. El collar envía una lectura por minuto cuando tiene señal GPS.
        </p>
      )}

      {!loading && located.length > 0 && withoutLocation.length > 0 && (
        <p className="legend-message">
          Sin ubicaciones todavía: {withoutLocation.map((p) => p.animal.tag_number).join(', ')}
        </p>
      )}
    </aside>
  )
}
