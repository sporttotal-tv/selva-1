import test from 'ava'
import { connect } from '../client/src/index'
import { start } from '../server/src/index'
import './assertions'
import { wait } from './assertions'
import getPort from 'get-port'

let srv
let port: number
test.before(async t => {
  port = await getPort()
  srv = await start({
    port
  })
  await wait(1500)
  const client = connect({ port })
  await client.updateSchema({
    languages: ['en'],
    types: {
      league: {
        prefix: 'le',
        fields: {
          name: { type: 'string', search: { type: ['TAG'] } }
        }
      },
      team: {
        prefix: 'te',
        fields: {
          name: { type: 'string', search: { type: ['TAG'] } }
        }
      },
      match: {
        prefix: 'ma',
        fields: {
          name: { type: 'string', search: { type: ['TAG'] } },
          value: { type: 'number', search: { type: ['NUMERIC', 'SORTABLE'] } },
          status: { type: 'number', search: { type: ['NUMERIC', 'SORTABLE'] } },
          date: { type: 'number', search: { type: ['NUMERIC', 'SORTABLE'] } }
        }
      }
    }
  })
})

test.after(async _t => {
  const client = connect({ port })
  const d = Date.now()
  await client.delete('root')
  console.log('removed', Date.now() - d, 'ms')
  await client.destroy()
  await srv.destroy()
})

test.serial('subscription find', async t => {
  const client = connect(
    {
      port
    },
    { loglevel: 'info' }
  )

  const matches = []
  const teams = []

  for (let i = 0; i < 100; i++) {
    teams.push({
      $id: await client.id({ type: 'team' }),
      name: 'team ' + i,
      type: 'team'
    })
  }

  for (let i = 0; i < 10; i++) {
    matches.push({
      name: 'match ' + i,
      type: 'match',
      value: i,
      parents: {
        $add: [
          teams[~~(Math.random() * teams.length)].$id,
          teams[~~(Math.random() * teams.length)].$id
        ]
      },
      status: i < 5 ? 100 : 300
    })
  }

  await Promise.all(teams.map(t => client.set(t)))

  const league = await client.set({
    type: 'league',
    name: 'league 1',
    children: matches
  })

  // if not id id = root
  const result = await client.get({
    // add id as well
    $includeMeta: true,
    items: {
      name: true,
      id: true,
      $list: {
        $find: {
          $traverse: 'descendants',
          $filter: [
            {
              $field: 'type',
              $operator: '=',
              $value: 'match'
            },
            {
              $field: 'value',
              $operator: '..',
              $value: [5, 10]
            }
          ]
        }
      }
    }
  })

  console.dir(result.$meta, { depth: 100 })

  // teams
  // league

  const result2 = await client.get({
    $id: league,
    $includeMeta: true,
    items: {
      name: true,
      id: true,
      $list: {
        $find: {
          $traverse: 'children',
          $filter: [
            {
              $field: 'type',
              $operator: '=',
              $value: 'match'
            },
            {
              $field: 'value',
              $operator: '..',
              $value: [5, 10]
            }
          ]
        }
      }
    }
  })

  // then nested
  console.dir(result2.$meta, { depth: 100 })

  await wait(3e3)

  t.true(true)
})
