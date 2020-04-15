import test from 'ava'
import { connect } from '../src/index'
import { start } from '@saulx/selva-server'
import './assertions'
import { wait } from './assertions'
import getPort from 'get-port'

/*
cms cases:
- create a layout
- edit a layout
- remove a layout
- update data related to layout
app cases:
- subscribe to layout for id
*/

let srv
let port: number
test.before(async t => {
  port = await getPort()
  srv = await start({ port })
  await wait(500)
  const client = connect({ port })
  const fields = {
    name: { type: 'string', search: { type: ['TAG'] } },
    title: { type: 'text', search: true },
    description: { type: 'text', search: true },
    start: {
      type: 'timestamp',
      search: { type: ['NUMERIC', 'SORTABLE'] }
    },
    end: {
      type: 'timestamp',
      search: { type: ['NUMERIC', 'SORTABLE'] }
    }
  }

  const types = {
    dictionary: {
      prefix: 'di',
      fields: {
        dictionaryGroup: {
          type: 'object',
          properties: {
            dictionaryText: {
              type: 'text'
            }
          }
        }
      }
    },
    league: {
      prefix: 'le',
      fields
    },
    sport: {
      prefix: 'sp',
      fields
    },
    club: {
      prefix: 'cl',
      fields
    },
    team: {
      prefix: 'te',
      fields
    },
    match: {
      prefix: 'ma',
      fields
    }
  }

  // @ts-ignore
  fields.layout = {
    type: 'object',
    properties: [...Object.keys(types), 'root'].reduce((properties, type) => {
      properties[type] = {
        type: 'json'
      }
      return properties
    }, {})
  }

  await client.updateSchema({
    languages: ['en'],
    rootType: {
      // @ts-ignore
      fields
    },
    // @ts-ignore
    types: types
  })
})

test.after(async _t => {
  const client = connect({ port })
  await client.delete('root')
  await client.destroy()
  await srv.destroy()
})

test.serial('layout without inheritance or refs', async t => {
  const client = connect({ port })
  // creating data
  const match = await client.set({
    type: 'match',
    $language: 'en',
    title: 'title-0',
    description: 'description-0',
    start: Date.now() - 10000,
    end: Date.now() + 60 * 60 * 1000 * 2,
    layout: {
      match: {
        id: true,
        components: [
          {
            component: { $value: 'comp1-0' },
            title: {
              $field: 'title'
            },
            description: {
              $field: 'description'
            }
          }
        ]
      }
    }
  })

  // subscribe to layout of match, and update match page subscription
  let updateCount = 0
  let totalCount = 0
  let fireCount = 0
  let pageObs
  let pageSub
  let layout
  let query

  client
    .observe({
      $id: match,
      layout: true //{ $inherit: true } // change this to { $inherit: true } to break it
    })
    .then(layoutObs =>
      layoutObs.subscribe(async res => {
        if (pageSub) {
          pageSub.unsubscribe()
        }
        query = {
          $id: match,
          $language: 'en',
          ...res.layout.match
        }
        pageObs = await client.observe(query)
        pageSub = pageObs.subscribe(res => {
          layout = res
          fireCount++
        })
      })
    )

  while (updateCount <= 2) {
    updateCount++
    totalCount++
    await client.set({
      $id: match,
      layout: {
        match: {
          components: [
            {
              component: {
                $value: `comp1-${updateCount}`
              },
              title: {
                $field: 'title'
              },
              description: {
                $field: 'description'
              }
            }
          ]
        }
      }
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components[0].component, `comp1-${updateCount}`)
  }

  while (updateCount <= 4) {
    updateCount++
    totalCount++
    await client.set({
      $id: match,
      $language: 'en',
      title: `title-${updateCount}`,
      description: `description-${updateCount}`
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components[0].title, `title-${updateCount}`)
    t.is(layout.components[0].description, `description-${updateCount}`)
  }

  let compCount = 1
  while (compCount <= 2) {
    compCount++
    totalCount++
    await client.set({
      $id: match,
      layout: {
        match: {
          components: [
            ...query.components,
            {
              component: {
                $value: `comp${compCount}-${updateCount}`
              },
              title: {
                $field: 'title'
              },
              description: {
                $field: 'description'
              }
            }
          ]
        }
      }
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components.length, compCount)
    layout.components.forEach(({ title, description }) => {
      t.is(title, `title-${updateCount}`)
      t.is(description, `description-${updateCount}`)
    })
  }
})

test.serial('layout with refs', async t => {
  const client = connect({ port })
  // add some refs to the layout!
  const dictionary = await client.set({
    type: 'dictionary',
    $language: 'en',
    dictionaryGroup: {
      dictionaryText: `dictionary-0`
    }
  })

  const match = await client.set({
    type: 'match',
    $language: 'en',
    title: 'title-0',
    description: 'description-0',
    start: Date.now() - 10000,
    end: Date.now() + 60 * 60 * 1000 * 2,
    layout: {
      match: {
        components: [
          {
            component: {
              $value: `comp-with-refs`
            },
            title: {
              $field: {
                value: {
                  $id: dictionary,
                  dictionaryGroup: { dictionaryText: true }
                },
                path: ['dictionaryGroup.dictionaryText']
              }
            },
            description: {
              $field: 'description'
            }
          }
        ]
      }
    }
  })

  // subscribe to layout of match, and update match page subscription
  let totalCount = 1
  let fireCount = 0
  let pageObs
  let pageSub
  let layout

  client
    .observe({
      $id: match,
      layout: true //{ $inherit: true } // change this to { $inherit: true } to break it
    })
    .then(layoutObs =>
      layoutObs.subscribe(async res => {
        if (pageSub) {
          pageSub.unsubscribe()
        }
        pageObs = await client.observe({
          $id: match,
          $language: 'en',
          ...res.layout.match
        })
        pageSub = pageObs.subscribe(res => {
          layout = res
          fireCount++
        })
      })
    )

  await wait(100)
  t.is(totalCount, fireCount)
  t.is(layout.components[0].title, 'dictionary-0')

  let dictionaryCount = 0
  while (dictionaryCount <= 4) {
    dictionaryCount++
    totalCount++
    await client.set({
      $id: dictionary,
      $language: 'en',
      dictionaryGroup: {
        dictionaryText: `dictionary-${dictionaryCount}`
      }
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components[0].title, `dictionary-${dictionaryCount}`)
  }
})

test.serial('layout using $inherit: true - own layout', async t => {
  const client = connect({ port })
  // creating data
  const match = await client.set({
    type: 'match',
    $language: 'en',
    title: 'title-0',
    description: 'description-0',
    start: Date.now() - 10000,
    end: Date.now() + 60 * 60 * 1000 * 2,
    layout: {
      match: {
        id: true,
        components: [
          {
            component: { $value: 'comp1-0' },
            title: {
              $field: 'title'
            },
            description: {
              $field: 'description'
            }
          }
        ]
      }
    }
  })

  // subscribe to layout of match, and update match page subscription
  let updateCount = 0
  let totalCount = 0
  let fireCount = 0
  let pageObs
  let pageSub
  let layout
  let query

  client
    .observe({
      $id: match,
      layout: { $inherit: true }
    })
    .then(layoutObs =>
      layoutObs.subscribe(async res => {
        if (pageSub) {
          pageSub.unsubscribe()
        }
        query = {
          $id: match,
          $language: 'en',
          ...res.layout.match
        }
        pageObs = await client.observe(query)
        pageSub = pageObs.subscribe(res => {
          layout = res
          fireCount++
        })
      })
    )

  while (updateCount <= 2) {
    updateCount++
    totalCount++
    await client.set({
      $id: match,
      layout: {
        match: {
          components: [
            {
              component: {
                $value: `comp1-${updateCount}`
              },
              title: {
                $field: 'title'
              },
              description: {
                $field: 'description'
              }
            }
          ]
        }
      }
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components[0].component, `comp1-${updateCount}`)
  }

  while (updateCount <= 4) {
    updateCount++
    totalCount++
    await client.set({
      $id: match,
      $language: 'en',
      title: `title-${updateCount}`,
      description: `description-${updateCount}`
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components[0].title, `title-${updateCount}`)
    t.is(layout.components[0].description, `description-${updateCount}`)
  }

  let compCount = 1
  while (compCount <= 2) {
    compCount++
    totalCount++
    await client.set({
      $id: match,
      layout: {
        match: {
          components: [
            ...query.components,
            {
              component: {
                $value: `comp${compCount}-${updateCount}`
              },
              title: {
                $field: 'title'
              },
              description: {
                $field: 'description'
              }
            }
          ]
        }
      }
    })
    await wait(100)
    t.is(totalCount, fireCount)
    t.is(layout.components.length, compCount)
    layout.components.forEach(({ title, description }) => {
      t.is(title, `title-${updateCount}`)
      t.is(description, `description-${updateCount}`)
    })
  }
})
