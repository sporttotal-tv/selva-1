import { promisify } from 'util'
import test from 'ava'
import { connect } from '../src/index'
import { start, startReplica } from '@saulx/selva-server'
import redis, {RedisClient, ReplyError} from 'redis'
import './assertions'
import {wait} from './assertions'
import getPort from 'get-port'

let srv
let port: number
let replica
let rclientOrigin: RedisClient | null = null
let rclientReplica: RedisClient | null = null

test.before(async t => {
  port = await getPort()
  srv = await start({
    port
  })

  replica = await startReplica({
    registry: { port },
    default: true
  })
  await new Promise((resolve, _reject) => {
    setTimeout(resolve, 100)
  })
})

test.beforeEach(async t => {
  const client = connect({ port }, { loglevel: 'info' })

  await client.redis.flushall({ type: 'origin' })
  await client.updateSchema({
    languages: ['en', 'de', 'nl'],
    rootType: {
      fields: {
        value: { type: 'number' },
        value1: { type: 'number' },
        nested: {
          type: 'object',
          properties: {
            fun: { type: 'string' }
          }
        }
      }
    },
    types: {
      match: {
        prefix: 'ma',
        fields: {
          title: { type: 'text' },
          value: { type: 'number' },
          description: { type: 'text' }
        }
      }
    }
  })

  // A small delay is needed after setting the schema
  await new Promise(r => setTimeout(r, 100))

  await client.destroy()

  rclientOrigin = redis.createClient(replica.origin.port)
  rclientReplica = redis.createClient(replica.port)
})

test.after(async _t => {
  const client = connect({ port })
  await client.destroy()
  await replica.destroy()
  await srv.destroy()
})

test.afterEach(async () => {
  if (rclientOrigin) {
    rclientOrigin.end(true)
    rclientOrigin = null
    rclientReplica.end(true)
    rclientReplica = null
  }
})

test.serial('verify basic replication', async t => {
  const infoOrigin = await new Promise((resolve, reject) => rclientOrigin.info((err, res) => err ? reject(err) : resolve(res)))
  const infoReplica = await new Promise((resolve, reject) => rclientReplica.info((err, res) => err ? reject(err) : resolve(res)))

  // @ts-ignore
  t.assert(infoOrigin.includes('role:master'), 'Origin has the correct Redis role')
  // @ts-ignore
  t.assert(infoReplica.includes('role:slave'), 'Replica has the correct Redis role')

  await new Promise((resolve, reject) => rclientOrigin.set('xyz', '1', (err, res) => err ? reject(err) : resolve(res)))
  await wait(20)
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.get('xyz', (err, res) => err ? reject(err) : resolve(res))),
    '1'
  )
})

test.serial('hierarchy replication', async t => {
  await Promise.all([
    new Promise((resolve, reject) => rclientOrigin.send_command('selva.hierarchy.add', ['___selva_hierarchy', 'grphnode_a', 'root'], (err, res) => err ? reject(err) : resolve(res))),
    new Promise((resolve, reject) => rclientOrigin.send_command('selva.hierarchy.add', ['___selva_hierarchy', 'grphnode_b', 'root'], (err, res) => err ? reject(err) : resolve(res)))
  ])
  await wait(20)
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.send_command('selva.hierarchy.find', ['___selva_hierarchy', 'bfs', 'descendants', 'root'], (err, res) => err ? reject(err) : resolve(res))),
    ['grphnode_a', 'grphnode_b']
  )

  await new Promise((resolve, reject) => rclientOrigin.send_command('selva.hierarchy.del', ['___selva_hierarchy', 'grphnode_a'], (err, res) => err ? reject(err) : resolve(res)))
  await wait(20)
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.send_command('selva.hierarchy.find', ['___selva_hierarchy', 'bfs', 'descendants', 'root'], (err, res) => err ? reject(err) : resolve(res))),
    ['grphnode_b']
  )
})

test.serial('modify command is replicated', async t => {
  const res1 = await new Promise((resolve, reject) => rclientOrigin.send_command('selva.modify', ['grphnode_a', '', '0', 'value', '5', '0', 'value1', '100'], (err, res) => err ? reject(err) : resolve(res)))
  t.deepEqual(res1, [ 'grphnode_a', 'UPDATED', 'UPDATED' ])
  await wait(200)
  const keys = await new Promise((resolve, reject) => rclientReplica.keys('*', (err, res) => err ? reject(err) : resolve(res))) as string[]
  t.assert(keys.includes('grphnode_a'))
  t.deepEqual(
    await new Promise((resolve, reject) => rclientOrigin.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5',
      value1: '100',
    }
  )
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5',
      value1: '100',
    }
  )

  // Only one update
  const res2 = await new Promise((resolve, reject) => rclientOrigin.send_command('selva.modify', ['grphnode_a', '', '0', 'value', '5', '0', 'value1', '2'], (err, res) => err ? reject(err) : resolve(res)))
  t.deepEqual(res2, [ 'grphnode_a', 'OK', 'UPDATED' ])
  await wait(200)
  t.deepEqual(
    await new Promise((resolve, reject) => rclientOrigin.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5',
      value1: '2',
    }
  )
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5',
      value1: '2',
    }
  )
})

test.serial('modify command is replicated ignoring errors', async t => {
  const res1 = await new Promise((resolve, reject) => rclientOrigin.send_command('selva.modify', ['grphnode_a', '', '0', 'value', '5', '127', 'value1', '100'], (err, res) => err ? reject(err) : resolve(res)))
  t.deepEqual(res1[1], 'UPDATED');
  t.assert(res1[2] instanceof ReplyError)

  await wait(200)

  const keys = await new Promise((resolve, reject) => rclientReplica.keys('*', (err, res) => err ? reject(err) : resolve(res))) as string[]
  t.assert(keys.includes('grphnode_a'))

  t.deepEqual(
    await new Promise((resolve, reject) => rclientOrigin.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5'
    }
  )
  t.deepEqual(
    await new Promise((resolve, reject) => rclientReplica.send_command('hgetall', ['grphnode_a'], (err, res) => err ? reject(err) : resolve(res))),
    {
      $id: 'grphnode_a',
      value: '5'
    }
  )
})