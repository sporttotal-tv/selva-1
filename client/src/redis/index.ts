import { SelvaClient, ServerType, connect } from '../'
import {
  ClientOpts,
  ConnectOptions,
  ServerSelector,
  ServerDescriptor
} from '../types'
import { RedisCommand, Servers, ServersById } from './types'
import RedisMethods from './methods'
import { v4 as uuidv4 } from 'uuid'
import { GetSchemaResult } from '../schema/types'
import { getClient, Client, addCommandToQueue } from './clients'
import getSchema from './getSchema'
import connectRegistry from './connectRegistry'

// now connect to registry make make
// re attach to different clients if they stop working

type Callback = (payload: any) => void

// add schema handling subscriptions / unsubscribe destorying making clients

class RedisSelvaClient extends RedisMethods {
  public selvaClient: SelvaClient

  public queue: { command: RedisCommand; selector: ServerSelector }[] = []
  public listenerQueue: {
    selector: ServerSelector
    event: string
    callback: Callback
  }[] = []

  public registry: Client

  public servers: Servers

  public serversById: ServersById

  public id: string

  constructor(
    selvaClient: SelvaClient,
    connectOptions: ConnectOptions,
    opts: ClientOpts
  ) {
    super()
    this.id = uuidv4()
    this.selvaClient = selvaClient
    connectRegistry(this, connectOptions)
  }

  async getServerDescriptor(
    selector: ServerSelector
  ): Promise<ServerDescriptor> {
    const retry = (): Promise<ServerDescriptor> =>
      new Promise((resolve, reject) => {
        this.registry.once('servers_updated', () => {
          resolve(this.getServerDescriptor(selector))
        })
      })

    if (!this.servers) {
      return retry()
    }
    if (selector.host && selector.port) {
      const server = this.serversById[`${selector.host}:${selector.port}`]
      if (!server) {
        return retry()
      }
      return server
    }
    if (!selector.name) {
      if (selector.type === 'registry') {
        selector.name = 'registry'
      } else {
        selector.name = 'default'
      }
    }
    if (!selector.type) {
      if (selector.name === 'registry') {
        selector.type = 'registry'
      } else {
        selector.type = 'origin'
      }
    }

    if (
      !this.servers[selector.name] ||
      !this.servers[selector.name][selector.type]
    ) {
      return retry()
    }

    const servers = this.servers[selector.name][selector.type]
    const server = servers[Math.floor(Math.random() * servers.length)]

    console.log('hello', server)
    return server
  }

  async getSchema(selector: ServerSelector): Promise<GetSchemaResult> {
    return getSchema(this, selector)
  }

  on(selector: ServerSelector, event: string, callback: Callback): void
  on(event: string, callback: Callback): void
  on(selector: any, event: any, callback?: any): void {
    if (!this.registry) {
      this.listenerQueue.push({ selector, event, callback })
    } else {
      if (typeof selector === 'string') {
        callback = event
        event = selector
        // if replica is available
        selector = { name: 'default', type: 'replica' }
      }

      if (selector.type === 'registry') {
        this.registry.subscriber.on(event, callback)
      } else {
      }
    }
  }

  addCommandToQueue(
    command: RedisCommand,
    selector: ServerSelector = { name: 'default' }
  ) {
    if (!this.registry) {
      this.queue.push({ command, selector })
    } else {
      if (selector.type === 'registry') {
        addCommandToQueue(this.registry, command)
      } else {
      }
    }
  }
}

export default RedisSelvaClient
