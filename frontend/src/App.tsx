import { HerdMap } from './components/HerdMap'
import { Legend } from './components/Legend'
import { useGeofence } from './hooks/useGeofence'
import { useHerdPositions } from './hooks/useHerdPositions'

export default function App() {
  const { geofence, error: geofenceError } = useGeofence()
  const { positions, lastUpdated, error, loading } = useHerdPositions()

  return (
    <div className="app">
      <header className="app-header">
        <h1>Ubicación del ganado</h1>
      </header>
      <main className="app-main">
        <HerdMap geofence={geofence} geofenceFailed={geofenceError !== null} positions={positions} />
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
