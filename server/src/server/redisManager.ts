import ProcessManager from './processManager'
import { SelvaClient, ServerType } from '@saulx/selva'

export default class RedisManager extends ProcessManager {
  private redisPort: number
  private redisHost: string
  private selvaClient: SelvaClient
  private type: ServerType
  private name: string

  constructor(
    args: string[],
    {
      host,
      port,
      selvaClient,
      type,
      name
    }: {
      host: string
      port: number
      selvaClient: SelvaClient
      name: string
      type: ServerType
    }
  ) {
    super('redis-server', args)

    this.redisHost = host
    this.redisPort = port
    this.selvaClient = selvaClient
    this.type = type
    this.name = name
  }

  protected async collect(): Promise<any> {
    const runtimeInfo = await super.collect()

    try {
      console.log(this.name, this.type, this.redisHost, this.redisPort)
      const info = await this.selvaClient.redis.info({
        port: this.redisPort,
        host: this.redisHost,
        type: this.type,
        name: this.name
      })

      if (info) {
        const infoLines = info.split('\r\n')
        const redisInfo = infoLines.reduce((acc, line) => {
          if (line.startsWith('#')) {
            return acc
          }

          const [key, val] = line.split(':')
          if (key === '') {
            return acc
          }

          return {
            ...acc,
            [key]: val
          }
        }, {})

        return { redisInfo, runtimeInfo }
      } else {
        return { isBusy: true, runtimeInfo }
      }
    } catch (err) {
      // store busy
      console.error('! cannot get info we may need to restart it!')
      return {
        redisInfo: {},
        runtimeInfo,
        err: err.message,
        isBusy: true
      }
    }
  }
}
