import { ConnectOptions, ServerDescriptor } from '../types'
import { getClient } from './clients'
import RedisSelvaClient from './'
import { REGISTRY_UPDATE } from '../constants'
import { Servers, ServersById } from './types'

const drainQueue = (client: RedisSelvaClient) => {
  client.queue.forEach(({ command, selector }) => {
    client.addCommandToQueue(command, selector)
  })
  client.listenerQueue.forEach(({ event, callback, selector }) => {
    client.on(selector, event, callback)
  })
  client.listenerQueue = []
  client.queue = []
}

const getServers = async (client: RedisSelvaClient) => {
  delete client.servers
  const serverList =
    (await client.smembers({ type: 'registry' }, 'servers')) || []
  const servers: Servers = {}
  const serversById: ServersById = {}
  const result: ServerDescriptor[] = await Promise.all(
    serverList.map(
      async (id: string): Promise<ServerDescriptor> => {
        const [host, port, name, type, def] = await client.hmget(
          { type: 'registry' },
          id,
          'host',
          'port',
          'name',
          'type',
          'default'
        )
        const descriptor = { host, port, name, type, default: def || false }
        serversById[id] = descriptor
        return descriptor
      }
    )
  )
  for (const server of result) {
    if (!servers[server.name]) {
      servers[server.name] = {}
      if (server.default) {
        servers.default = servers[server.name]
      }
    }
    if (!servers[server.name][server.type]) {
      servers[server.name][server.type] = []
    }
    servers[server.name][server.type].push(server)
  }
  client.servers = servers
  client.registry.emit('servers_updated', servers)
}

const createRegistryClient = (
  client: RedisSelvaClient,
  port: number,
  host: string
) => {
  client.registry = getClient(client, 'registry', 'registry', port, host)
  client.subscribe({ type: 'registry' }, REGISTRY_UPDATE)
  client.on({ type: 'registry' }, 'message', channel => {
    if (channel === REGISTRY_UPDATE) {
      console.log('REGISTRY UPDATED (could be a new client!')
      getServers(client)
    }
  })
  getServers(client)
}

const connectRegistry = (
  client: RedisSelvaClient,
  connectOptions: ConnectOptions
) => {
  if (typeof connectOptions === 'function') {
    let prevConnectOptions
    connectOptions().then(parsedConnectOptions => {
      prevConnectOptions = parsedConnectOptions
      createRegistryClient(
        client,
        parsedConnectOptions.port,
        parsedConnectOptions.host
      )
      const dcHandler = async () => {
        const newConnectionOptions = await connectOptions()
        if (
          newConnectionOptions.host !== prevConnectOptions.host ||
          newConnectionOptions.port !== prevConnectOptions.port
        ) {
          client.registry.removeListener('disconnect', dcHandler)
          client.registry = undefined
          connectRegistry(client, connectOptions)
        }
      }
      client.registry.on('disconnect', dcHandler)
      drainQueue(client)
    })
  } else if (connectOptions instanceof Promise) {
    connectOptions.then(parsedConnectOptions => {
      createRegistryClient(
        client,
        parsedConnectOptions.port,
        parsedConnectOptions.host
      )
      drainQueue(client)
    })
  } else {
    console.log('start with non async connect')
    createRegistryClient(client, connectOptions.port, connectOptions.host)
    drainQueue(client)
  }
}

export default connectRegistry
