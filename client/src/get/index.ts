import { SelvaClient } from '..'
import { GetResult, GetOptions, ObserveEventOptions } from './types'
import createGetOperations from './createGetOperations'
import executeGetOperations, { adler32 } from './executeGetOperations'
import resolveId from './resolveId'
import combineResults from './combineResults'
import { createRpn } from '@sporttotal/selva-query-ast-parser'

// add this later!
import validate, {
  ExtraQueries,
  ExtraQuery,
  PostGetExtraQuery,
} from './validate'

// move to saulx/utils
import { deepMerge } from '@sporttotal/utils'
import { ServerDescriptor } from '~selva/types'

function getExtraQueriesByField(
  extraQueries: ExtraQueries
): Record<string, ExtraQuery> {
  const map: Record<string, ExtraQuery> = {}
  for (const db in extraQueries) {
    for (const q of extraQueries[db]) {
      map[q.path] = q
    }
  }
  return map
}

function makeNewGetOptions(
  extraQueries: Record<string, ExtraQuery>,
  getOpts: GetOptions,
  path: string = ''
): GetOptions {
  if (Object.keys(extraQueries).length === 0) {
    return getOpts
  }

  const newOpts = {}
  for (const key in getOpts) {
    const newPath = path + '.' + key
    if (extraQueries[newPath]) {
      const extraQuery: ExtraQuery = extraQueries[newPath]
      if (extraQuery.type === 'traverse') {
        newOpts[key] = extraQuery.value || []
      } else if (extraQuery.type === 'text_search') {
        // TODO: in full
        // here we gotta somehow convert the text search results into a $traverse and/or filter and/or something that makes sense in our new query
        // TODO: add $db support? fun!
      } else {
        newOpts[key] = extraQuery.placeholder
      }
    } else if (
      !key.startsWith('$') &&
      key !== 'path' &&
      Array.isArray(getOpts[key])
    ) {
      newOpts[key] = getOpts[key].map((g, i) => {
        const extraQuery: PostGetExtraQuery = <PostGetExtraQuery>(
          extraQueries[newPath + '.' + i]
        )

        if (extraQuery) {
          return extraQuery.placeholder
        }

        return makeNewGetOptions(extraQueries, g, newPath + '.' + i)
      })
    } else if (Array.isArray(getOpts[key])) {
      newOpts[key] = getOpts[key]
    } else if (typeof getOpts[key] === 'object') {
      newOpts[key] = makeNewGetOptions(extraQueries, getOpts[key], newPath)
    } else {
      newOpts[key] = getOpts[key]
    }
  }

  return newOpts
}

async function get(
  client: SelvaClient,
  props: GetOptions,
  meta?: any,
  nested: boolean = false
): Promise<GetResult> {
  // -------------
  // // this is where we collect the dbs and  which initialize schemas to do after
  // TODO: need to intialize for each db!

  const db = props.$db || 'default'
  let subId = props.$subscription

  let originDescriptors: Record<string, ServerDescriptor> = {}
  if (subId) {
    originDescriptors = props.$originDescriptors || {}
  }

  const extraQueries: ExtraQueries = {}
  await validate(extraQueries, client, props)
  const newProps = makeNewGetOptions(
    getExtraQueriesByField(extraQueries),
    props
  )

  await client.initializeSchema({ $db: db })

  const id = await resolveId(client, newProps)

  if (!id) {
    if (subId) {
      const ids = []

      if (newProps.$alias)
        ids.push(
          ...(Array.isArray(newProps.$alias)
            ? newProps.$alias
            : [newProps.$alias])
        )
      if (newProps.$id)
        ids.push(
          ...(Array.isArray(newProps.$id) ? newProps.$id : [newProps.$id])
        )

      await client.redis.selva_subscriptions_addmissing(
        { name: db },
        '___selva_hierarchy',
        subId,
        ...ids
      )
    }
    return { $isNull: true }
  }

  if (newProps.$trigger && !newProps.$subscription) {
    // $trigger only works with subscriptions
    return { $isNull: true }
  } else if (newProps.$trigger && newProps.$id) {
    delete newProps.$trigger
    delete newProps.$subscription
    subId = null
  } else if (newProps.$trigger) {
    const rpn = newProps.$trigger.$filter
      ? createRpn(newProps.$trigger.$filter) || []
      : []
    await client.redis.selva_subscriptions_addtrigger(
      originDescriptors[db] || { name: db },
      '___selva_hierarchy',
      subId,
      adler32({
        type: 'trigger-' + newProps.$trigger.$event,
        fields: [],
        id: 'root',
        rpn,
      }),
      newProps.$trigger.$event,
      ...rpn
    )

    await client.redis.selva_subscriptions_refresh(
      originDescriptors[db] || { name: db },
      '___selva_hierarchy',
      subId
    )

    return { $ignore: true }
  }

  const lang = newProps.$language
  let resultMeta: any = {}

  const getResult = await executeGetOperations(
    client,
    lang,
    { db, subId, meta: resultMeta, originDescriptors },
    createGetOperations(client, newProps, id, '', db)
  )

  // maybe ncie function?
  if (meta || props.$includeMeta) {
    if (!meta) {
      if (!resultMeta) {
        resultMeta = {}
      }
      meta = { [props.$db || 'default']: resultMeta }
      meta.___refreshAt = resultMeta.___refreshAt
    } else {
      if (resultMeta.___refreshAt) {
        if (!meta.___refreshAt || meta.___refreshAt > resultMeta.___refreshAt) {
          meta.___refreshAt = resultMeta.___refreshAt
        }
      }
      deepMerge(meta, {
        [props.$db || 'default']: resultMeta,
      })
    }
  }

  await combineResults(client, extraQueries, props.$language, getResult, meta)

  if (props.$includeMeta && !nested) {
    getResult.$meta = meta
  }

  return getResult
}

export { get, GetResult, GetOptions, ObserveEventOptions }
