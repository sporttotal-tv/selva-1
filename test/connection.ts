import test from 'ava'
import { connect } from '../src/index'
import { start } from 'selva-server'
import { wait } from './assertions'

test('Connect and re-connect', async t => {
  let current = { port: 6066 }

  const client = await connect(async () => {
    console.log('ASYNC connect it')
    return current
  })

  const server = await start({ port: 6066, modules: ['redisearch'] })

  client.set({
    $id: 'cuflap',
    title: {
      en: 'lurkert'
    }
  })

  // add these!!!
  // client.isConnected
  // client.on('connect', () => {})
  // normally use subscribe for this kind of stuff

  await wait(500)

  t.deepEqual(
    await client.get({
      $id: 'cuflap',
      title: true
    }),
    { title: { en: 'lurkert' } }
  )

  console.log('destroy!')
  await server.destroy()

  console.log('destroyed!')

  await wait(1e3)
  current = { port: 6067 }
  const server2 = await start({ port: 6067, modules: ['redisearch'] })

  t.deepEqual(
    await client.get({
      $id: 'cuflap',
      title: true
    }),
    {}
  )
})
