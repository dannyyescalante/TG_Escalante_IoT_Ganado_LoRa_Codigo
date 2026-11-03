import { HerdMap } from './components/HerdMap'
import { Legend } from './components/Legend'
import { POLL_INTERVAL_MS } from './config'
import { useGeofence } from './hooks/useGeofence'
import { useHerdPositions } from './hooks/useHerdPositions'

export default function App() {
  const { geofence, error: geofenceError } = useGeofence()
  const { positions, lastUpdated, error, loading } = useHerdPositions()

  return (
    <div className="app">
      <header className="app-header">
        <h1>Collar IoT — Ubicación del ganado</h1>
        <p className="app-subtitle">Monitoreo por geocerca · se actualiza cada {POLL_INTERVAL_MS / 1000} s</p>
      </header>
      <main className="app-main">
        <HerdMap
          geofence={geofence}
          geofenceFailed={geofenceError !== null}
          positions={positions}
          positionsLoaded={!loading}
          lastUpdated={lastUpdated}
        />
        <Legend
          positions={positions}
          lastUpdated={lastUpdated}
          error={error}
          loading={loading}
          geofence={geofence}
          geofenceError={geofenceError}
        />
      </main>
    </div>
  )
}
