import test from 'ava'
import { connect } from '../src/index'
import { start, s3Backups } from '@saulx/selva-server'
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
  const client = connect({ port })
  await client.updateSchema({
    languages: ['en', 'de'],
    types: {
      sport: {
        prefix: 'sp'
      },
      club: {
        prefix: 'cl'
      },
      competition: {
        prefix: 'co'
      },
      team: {
        prefix: 'te',
        hierarchy: {
          club: {
            excludeAncestryWith: ['sport']
          }
        }
      },
      match: {
        prefix: 'ma',
        hierarchy: {
          team: {
            // this messes it up!
            excludeAncestryWith: ['competition']
          }
        },
        fields: {
          video: {
            type: 'record',
            meta: { type: 'video' },
            values: {
              type: 'object',
              properties: {
                mp4: {
                  type: 'url'
                },
                streamId: {
                  type: 'string'
                },
                hls: {
                  type: 'url',
                  meta: {
                    upload: 'video'
                  }
                }
              }
            }
          }
        }
      }
    }
  })

  await client.destroy()
})

test.after(async t => {
  const client = connect({ port })
  await client.delete('root')
  await client.destroy()
  await srv.destroy()
  await t.connectionsAreEmpty()
})

test.serial('correct hierachy rules', async t => {
  const client = connect({ port }, { loglevel: 'info' })

  const football = await client.set({
    type: 'sport'
  })

  const bball = await client.set({
    type: 'sport'
  })

  const eredivisie2020 = await client.set({
    type: 'competition',
    parents: [football]
  })

  const ajax = await client.set({
    type: 'club',
    parents: [football, bball]
  })

  const ajax1 = await client.set({
    type: 'team',
    parents: [ajax, eredivisie2020]
  })

  const ajax2 = await client.set({
    type: 'team',
    parents: [ajax, eredivisie2020]
  })

  const match = await client.set({
    type: 'match',
    parents: [ajax1, ajax2, eredivisie2020],
    video: {
      default: {
        // hls: 'https://google.com',
        mp4: 'https://google.com'
      },
      pano: {}
    }
  })

  t.deepEqualIgnoreOrder(await client.get({ $id: match, ancestors: true }), {
    ancestors: ['root', football, eredivisie2020, ajax1, ajax2, ajax]
  })

  try {
    await client.get({
      children: {
        $list: {
          $limit: 10,
          $find: {
            $traverse: 'descendants',
            $filter: [
              {
                $value: 'match',
                $field: 'type',
                $operator: '='
              }
            ]
          }
        },
        video: true
      }
    })

    t.pass()
  } catch (e) {
    t.fail('Error getting descendants ' + e.message)
  }

  await client.delete('root')
  await client.destroy()
})
