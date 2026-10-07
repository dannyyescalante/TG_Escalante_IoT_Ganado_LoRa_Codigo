import { useEffect, useState } from 'react'
import { getGeofence } from '../api/client'
import type { Geofence } from '../types'

// La geocerca es fija (sin edicion dinamica): se pide una sola vez.
export function useGeofence() {
  const [geofence, setGeofence] = useState<Geofence | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    let cancelled = false
    getGeofence()
      .then((result) => {
        if (!cancelled) setGeofence(result)
      })
      .catch((err: Error) => {
        if (!cancelled) setError(err.message)
      })
    return () => {
      cancelled = true
    }
  }, [])

  return { geofence, error }
}
