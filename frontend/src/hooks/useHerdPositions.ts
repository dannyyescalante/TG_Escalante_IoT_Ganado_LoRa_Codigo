import { useEffect, useState } from 'react'
import { getAnimals, getEvents, getGeofence, getLatestLocation } from '../api/client'
import { POLL_INTERVAL_MS } from '../config'
import type { Animal, GeofenceEvent, LocationReading } from '../types'

export interface HerdPosition {
  animal: Animal
  location: LocationReading | null
  lastEvent: GeofenceEvent | null
}

interface HerdPositionsState {
  positions: HerdPosition[]
  lastUpdated: Date | null
  error: string | null
  loading: boolean
}

// "Fuera" = salida confirmada (RF18/RF27) aun sin un "enter" posterior, igual que las alertas de Telegram.
// No se usa inside_geofence de la ultima lectura: una sola lectura fuera puede ser ruido del GPS.
export function isConfirmedOutside(position: HerdPosition): boolean {
  return position.lastEvent?.event_type === 'exit'
}

async function fetchHerdPositions(): Promise<HerdPosition[]> {
  const [animals, events, geofence] = await Promise.all([getAnimals(), getEvents(), getGeofence()])
  const activeAnimals = animals.filter((animal) => animal.is_active)

  // /events viene ordenado por occurred_at DESC: el primero de cada animal es su evento mas reciente.
  // Solo cuentan los de la geocerca activa: un exit de una geocerca anterior no dice nada de la actual.
  const lastEventByAnimal = new Map<number, GeofenceEvent>()
  for (const event of events) {
    if (event.geofence_id === geofence.id && !lastEventByAnimal.has(event.animal_id)) {
      lastEventByAnimal.set(event.animal_id, event)
    }
  }

  const locations = await Promise.all(activeAnimals.map((animal) => getLatestLocation(animal.id)))

  return activeAnimals.map((animal, index) => ({
    animal,
    location: locations[index],
    lastEvent: lastEventByAnimal.get(animal.id) ?? null,
  }))
}

export function useHerdPositions(): HerdPositionsState {
  const [state, setState] = useState<HerdPositionsState>({
    positions: [],
    lastUpdated: null,
    error: null,
    loading: true,
  })

  useEffect(() => {
    // En desarrollo StrictMode monta el efecto dos veces: el cleanup debe cortar el ciclo anterior.
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | undefined

    // setTimeout encadenado: el siguiente ciclo empieza cuando termina el actual, sin solaparse.
    async function poll() {
      try {
        const positions = await fetchHerdPositions()
        if (cancelled) return
        setState({ positions, lastUpdated: new Date(), error: null, loading: false })
      } catch (err) {
        if (cancelled) return
        // Se conservan los ultimos datos buenos: un corte momentaneo no vacia el mapa.
        setState((previous) => ({ ...previous, error: (err as Error).message, loading: false }))
      }
      if (!cancelled) {
        timer = setTimeout(poll, POLL_INTERVAL_MS)
      }
    }

    poll()
    return () => {
      cancelled = true
      clearTimeout(timer)
    }
  }, [])

  return state
}
