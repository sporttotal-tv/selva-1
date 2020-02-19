import { createClient, RedisClient } from 'redis'
import { SelvaClient } from '../../../client/src'
import { GetOptions } from '../../../client/src/get/types'
import { QuerySubscription } from '../../../lua/src/get/query/types'
import { Schema } from '../../../client/src/schema'
import { createHash } from 'crypto'
import addFields from './addFields'
import attach from './attach'

export default class SubscriptionManager {
  public refreshSubscriptionsTimeout: NodeJS.Timeout
  public lastRefreshed: Date
  public cleanUp: boolean = false
  public lastModifyEvent: number
  public queries: Record<string, QuerySubscription[]> = {}
  public inProgress: Record<string, true> = {}
  public subscriptions: Record<string, GetOptions> = {}
  public subscriptionsByField: Record<string, Set<string>> = {}
  public refsById: Record<string, Record<string, string>> = {}
  public lastResultHash: Record<string, string> = {}
  public lastHeartbeat: Record<string, number> = {}
  public client: SelvaClient
  public sub: RedisClient
  public pub: RedisClient

  cleanUpProgress() {
    if (!this.cleanUp) {
      this.cleanUp = true
      setTimeout(() => {
        this.inProgress = {}
        this.cleanUp = false
      }, 250)
    }
  }

  heartbeats() {
    this.pub.publish('___selva_events:heartbeat', '')

    for (const subscriptionId in this.subscriptions) {
      this.pub.publish(
        `___selva_subscription:${subscriptionId}`,
        JSON.stringify({ type: 'heartbeat' })
      )
    }
  }

  async attach(port: number) {
    this.client = new SelvaClient({ port }, { loglevel: 'off' })
    this.sub = createClient({ port })
    this.pub = createClient({ port })
    try {
      attach(this, port)
    } catch (err) {
      throw new Error(err)
    }
  }

  detach() {
    this.sub.end(false)
    this.sub = undefined

    this.pub.end(false)
    this.pub = undefined

    if (this.refreshSubscriptionsTimeout) {
      clearTimeout(this.refreshSubscriptionsTimeout)
      this.refreshSubscriptionsTimeout = undefined
    }

    this.lastRefreshed = undefined
    this.lastHeartbeat = {}
  }

  get closed(): boolean {
    return this.sub === undefined
  }

  async sendUpdate(
    subscriptionId: string,
    getOptions?: GetOptions,
    deleteOp: boolean = false
  ) {
    if (!this.pub) {
      return
    }

    if (deleteOp) {
      this.pub.publish(
        `___selva_subscription:${subscriptionId}`,
        JSON.stringify({ type: 'delete' }),
        (err, _reply) => {
          if (err) {
            console.error(err)
          }
        }
      )

      // delete cache for latest result since there is no result now
      delete this.lastResultHash[subscriptionId]
      return
    }

    getOptions = getOptions || this.subscriptions[subscriptionId]

    const payload = await this.client.get(
      Object.assign({}, getOptions, {
        $includeMeta: true
      })
    )

    const refs = payload.$meta.$refs
    delete this.refsById[getOptions.$id]

    let hasRefs = false
    // check if query is also there
    const newRefs: Record<string, string> = {}

    // needsRefresh

    for (const refSource in refs) {
      hasRefs = true
      const refTargets = refs[refSource]
      newRefs[refSource] = refTargets
    }
    this.refsById[getOptions.$id] = newRefs

    if (hasRefs) {
      this.refreshSubscription(subscriptionId)
    }

    if (payload.$meta.query) {
      // what do you need to refresh? just attach
      this.queries[subscriptionId] = <QuerySubscription[]>payload.$meta.query

      // console.log('ATTACH', subscriptionId, payload.$meta.query)
    }
    // need to clear $meta

    if (payload.$meta.query) {
      // console.log('REMOVE META FROM NESTED FIELDS')
    }

    // clean up refs before we send it to the client
    delete payload.$meta

    // hack-ish thing: include the result object in the string
    // so we don't need to encode/decode as many times

    // if query need to remove NESTED meta

    const resultStr = JSON.stringify({ type: 'update', payload })

    const currentHash = this.lastResultHash[subscriptionId]
    const hashingFn = createHash('sha256')
    hashingFn.update(resultStr)
    const newHash = hashingFn.digest('hex')

    // de-duplicate events
    // with this we can avoid sending events where nothing changed upon reconnection
    // both for queries and for gets by id
    if (currentHash && currentHash === newHash) {
      return
    }

    this.lastResultHash[subscriptionId] = newHash

    this.pub.publish(
      `___selva_subscription:${subscriptionId}`,
      resultStr,
      (err, _reply) => {
        // console.error(err)
      }
    )
  }

  public async refreshSubscription(
    subId: string,
    subs: Record<string, GetOptions> = this.subscriptions,
    fieldMap: Record<string, Set<string>> = this.subscriptionsByField,
    schema?: Schema,
    stored?: string,
    cleanup: boolean = false
  ) {
    // add special query stuff here come on do it
    if (!schema) {
      schema = (await this.client.getSchema()).schema
    }

    if (!stored) {
      stored = await this.client.redis.hget('___selva_subscriptions', subId)
    }

    const getOptions: GetOptions = JSON.parse(stored)

    if (cleanup && this.lastHeartbeat[subId]) {
      // if no heartbeats in two minutes, clean up
      if (Date.now() - this.lastHeartbeat[subId] > 1000 * 120) {
        delete this.lastHeartbeat[subId]
        delete this.lastResultHash[subId]

        await this.client.redis.hdel('___selva_subscriptions', subId)
        return
      }
    }

    const fields: Set<string> = new Set()
    subs[subId] = getOptions

    addFields('', fields, schema, getOptions)
    for (const field of fields) {
      let current = fieldMap[getOptions.$id + field]
      if (!current) {
        fieldMap[getOptions.$id + field] = current = new Set()
      }
      current.add(subId)
    }

    if (this.refsById[getOptions.$id]) {
      for (const refSource in this.refsById[getOptions.$id]) {
        let current = fieldMap[getOptions.$id + '.' + refSource]
        if (!current) {
          fieldMap[getOptions.$id + '.' + refSource] = current = new Set()
        }
        current.add(subId)
      }
    }
  }

  public recalculateUpdates() {
    for (const subId in this.subscriptions) {
      this.sendUpdate(subId).catch(e => {
        console.error(e)
      })
    }
  }

  public async refreshSubscriptions() {
    const schema = (await this.client.getSchema()).schema

    const lastEdited = await this.client.redis.hget(
      '___selva_subscriptions',
      '___lastEdited'
    )

    // only refresh if there are new changes to the subscription metadata
    if (lastEdited && this.lastRefreshed) {
      const d = new Date(lastEdited)
      if (d <= this.lastRefreshed) {
        return
      }
    }

    const stored = await this.client.redis.hgetall('___selva_subscriptions')
    if (!stored) {
      return
    }

    const fieldMap: Record<string, Set<string>> = {}
    const subs: Record<string, GetOptions> = {}
    for (const subscriptionId in stored) {
      if (subscriptionId.startsWith('___')) {
        // skip internal keys
        continue
      }

      this.refreshSubscription(
        subscriptionId,
        subs,
        fieldMap,
        schema,
        stored[subscriptionId],
        true
      )
    }

    this.lastRefreshed = new Date()

    this.subscriptionsByField = fieldMap
    this.subscriptions = subs
  }
}