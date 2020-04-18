import retry from 'async-retry'
import { spawn, execSync } from 'child_process'
import fs from 'fs'
import path from 'path'
import {
  BackupFns,
  scheduleBackups,
  saveAndBackUp,
  loadBackup
} from './backups'
import cleanExit from './cleanExit'
import SubscriptionManager from './subscriptions'

export * as s3Backups from './backup-plugins/s3'
export * as dropboxBackups from './backup-plugins/dropbox'

type Service = {
  port: number
  host: string
}

// make abstraction on top

export type Subscriptions = {
  port?: number | Promise<number>
  service?: Service | Promise<Service>
  host?: number | Promise<number>
  selvaServer?: {
    port?: number | Promise<number>
    service?: Service | Promise<Service>
    host?: string | Promise<string>
  }
}

type FnStart = {
  port?: number | Promise<number>
  service?: Service | Promise<Service>
  replica?: Service | Promise<Service>
  modules?: string[]
  verbose?: boolean
  backups?: {
    loadBackup?: boolean
    scheduled?: { intervalInMinutes: number }
    backupFns: BackupFns | Promise<BackupFns>
  }
  seperateSubsmanager?: boolean
  subscriptions?: Subscriptions | boolean
}

export type SelvaServer = {
  on: (
    type: 'log' | 'data' | 'close' | 'error',
    cb: (data: any) => void
  ) => void
  destroy: () => Promise<void>
  backup: () => Promise<void>
  openSubscriptions: () => Promise<void>
  closeSubscriptions: () => void
  subsManagerServer?: SelvaServer
}

const defaultModules = ['redisearch', 'selva']

const wait = (): Promise<void> =>
  new Promise(resolve => {
    setTimeout(resolve, 100)
  })

export const startInternal = async function({
  port: portOpt,
  service,
  modules,
  replica,
  verbose = false,
  backups = null,
  subscriptions,
  seperateSubsmanager
}: FnStart): Promise<SelvaServer> {
  let backupCleanup: () => void
  let port: number
  let backupFns: BackupFns
  if (verbose) console.info('Start db 🌈')
  if (service instanceof Promise) {
    if (verbose) {
      console.info('awaiting service')
    }
    service = await service

    if (verbose) {
      console.info('service', service)
    }
  }

  if (portOpt instanceof Promise) {
    if (verbose) {
      console.info('awaiting port')
    }
    port = await portOpt
  } else {
    port = portOpt
  }

  if (replica instanceof Promise) {
    if (verbose) {
      console.info('awaiting db to replicate')
    }
    replica = await replica
    if (verbose) {
      console.info('replica', replica)
    }
  }

  if (!port && service) {
    port = service.port
    if (verbose) {
      console.info('listen on port', port)
    }
  }

  const args = ['--port', String(port), '--protected-mode', 'no']

  if (backups) {
    if (backups.backupFns instanceof Promise) {
      backupFns = await backups.backupFns
    } else {
      backupFns = backups.backupFns
    }

    if (backups.loadBackup) {
      console.log('Loading backup')
      await loadBackup(process.cwd(), backupFns)
      console.log('Backup loaded')
    }

    if (backups.scheduled) {
      args.push('--save', '10', '1')

      backupCleanup = scheduleBackups(
        process.cwd(),
        backups.scheduled.intervalInMinutes,
        backupFns
      )
    }
  }

  if (modules) {
    modules = [...new Set([...defaultModules, ...modules])]
  } else {
    modules = defaultModules
  }

  modules.forEach(m => {
    const platform = process.platform + '_' + process.arch
    const p = path.join(
      __dirname,
      '../',
      'modules',
      'binaries',
      platform,
      m + '.so'
    )
    if (fs.existsSync(p)) {
      if (verbose) {
        console.info(`  Load redis module "${m}"`)
      }
      args.push('--loadmodule', p)
    } else {
      console.warn(`${m} module does not exists for "${platform}"`)
    }
  })

  if (replica) {
    args.push('--replicaof', replica.host, String(replica.port))
  }

  const tmpPath = path.join(process.cwd(), './tmp')
  if (!fs.existsSync(tmpPath)) {
    fs.mkdirSync(tmpPath)
  }

  try {
    const dir = args[args.indexOf('--dir') + 1]
    execSync(`redis-cli -p ${port} config set dir ${dir}`)
    execSync(`redis-cli -p ${port} shutdown`)
  } catch (e) {}

  const redisDb = spawn('redis-server', args)

  let subs

  if (subscriptions) {
    if (typeof subscriptions === 'object') {
      subs = new SubscriptionManager()

      console.log(`subs enabled ${subscriptions}`, port)

      if (seperateSubsmanager) {
        await subs.createServer(subscriptions, seperateSubsmanager)
      }

      // may need to create another server ":/"

      await subs.connect(subscriptions)
    }
  }

  const redisServer: SelvaServer = {
    on: (type: 'data' | 'close' | 'error', cb: (data: any) => void) => {
      if (type === 'error') {
        redisDb.stderr.on('data', cb)
      } else if (type === 'data') {
        redisDb.stdout.on('data', cb)
      } else {
        redisDb.on('close', cb)
      }
    },
    closeSubscriptions: () => {
      if (subs) {
        subs.destroy()
      }
    },
    openSubscriptions: async () => {
      if (subs && typeof subscriptions === 'object') {
        await subs.connect(subscriptions)
      }
    },
    destroy: async () => {
      execSync(`redis-cli -p ${port} shutdown`)
      redisDb.kill()
      if (backupCleanup) {
        backupCleanup()
      }

      if (subs) {
        await subs.destroy()
      }
      await wait()
    },
    backup: async () => {
      // make a manual backup if available
      if (!backupFns) {
        throw new Error(`No backup options supplied`)
      }

      await saveAndBackUp(process.cwd(), port, backupFns)
    }
  }

  cleanExit(port)

  if (verbose) console.info(`🌈 Succesfully started db on port ${port}`)

  return redisServer
}

export const start = async (opts: FnStart): Promise<SelvaServer> => {
  if (opts.subscriptions) {
    if (opts.subscriptions === true) {
      // prob want seperate thing to be the default
      opts.subscriptions = {
        port: opts.port,
        service: opts.service,
        selvaServer: {
          service: opts.service,
          port: opts.port
        }
      }
      return startInternal(opts)
    } else {
      if (!opts.port && !opts.service) {
        // just subs manager
        opts.port = opts.subscriptions.port
        opts.service = opts.subscriptions.service
        return startInternal(opts)
      } else {
        if (!opts.subscriptions.selvaServer) {
          opts.subscriptions.selvaServer = {
            service: opts.service,
            port: opts.port
          }
        }

        if (opts.subscriptions.port || opts.subscriptions.service) {
          opts.seperateSubsmanager = true
          opts.subscriptions.selvaServer = {
            service: opts.service,
            port: opts.port
          }
          // needs to create a seperate subs manager
          return startInternal(opts)
        } else {
          return startInternal(opts)
        }
      }
    }
  } else {
    if (opts.subscriptions === undefined) {
      opts.subscriptions = {
        port: opts.port,
        service: opts.service,
        selvaServer: {
          service: opts.service,
          port: opts.port
        }
      }
    }
    return startInternal(opts)
  }
}
