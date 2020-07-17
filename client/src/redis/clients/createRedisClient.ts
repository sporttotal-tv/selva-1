import { RedisClient } from 'redis'
import { Client } from './'
import { SERVER_HEARTBEAT } from '../../constants'

const createRedisClient = (
  client: Client,
  host: string,
  port: number,
  label: string
): RedisClient => {
  let tries = 0
  let retryTimer = 0
  let isConnected: boolean = false
  let isHarddc: boolean = false

  if (label === 'publisher') {
    client.startClientTimer = setTimeout(() => {
      if (!isHarddc) {
        clearTimeout(client.serverHeartbeat)
        // console.log('cannot get it ready!', host, port, label)
        isHarddc = true
        client.emit('hard-disconnect')
      }
    }, 60e3)
  }

  const retryStrategy = () => {
    if (tries > 10) {
      clearTimeout(client.serverHeartbeat)
      clearTimeout(client.startClientTimer)
      if (label === 'publisher' && !isHarddc) {
        // console.log('HARD DC')
        isHarddc = true
        client.emit('hard-disconnect')
      }
    } else {
      if (tries === 0 && isConnected === true) {
        isConnected = false
        if (label === 'publisher') {
          client.emit('disconnect', label)
        }
      }
    }
    tries++
    if (retryTimer < 1e3) {
      retryTimer += 100
    }
    // redisClient.emit('error', error)
    return retryTimer
  }

  const redisClient = new RedisClient({
    port,
    host,
    retry_strategy: retryStrategy
  })

  // hard disconnects emitted from hacky monkey patching on ready check
  redisClient.on('hard-disconnect', () => {
    isConnected = false
    isHarddc = true
    client.emit('hard-disconnect')
  })

  if (label === 'subscriber') {
    redisClient.on('message', channel => {
      if (channel === SERVER_HEARTBEAT && !isHarddc) {
        clearTimeout(client.serverHeartbeat)
        client.serverHeartbeat = setTimeout(() => {
          console.error('🔥 heartbeat expired disconnect it!')
          isHarddc = true
          client.emit('hard-disconnect')
        }, 5 * 60e3) //  5 * 60e3
      }
    })
  }

  redisClient.setMaxListeners(1e4)

  redisClient.on('ready', () => {
    isHarddc = false
    if (label === 'publisher') {
      clearTimeout(client.startClientTimer)
    }
    tries = 0
    retryTimer = 0
    isConnected = true
    if (label === 'publisher') {
      client.emit('connect', label)
    }
  })

  redisClient.on('error', err => {
    if (err.code === 'ECONNREFUSED') {
      isConnected = false
      if (label === 'publisher' && isConnected) {
        client.emit('disconnect', label)
      }
    } else {
      client.emit('error', err)
    }
  })

  return redisClient
}

export default createRedisClient
