import { SelvaClient } from '../'
import { Schema, TypesDb, SearchIndexes } from '.'

type GetSchemaResult = {
  types: TypesDb
  schema: Schema
  searchIndexes: SearchIndexes
}

async function getSchema(client: SelvaClient): Promise<GetSchemaResult> {
  const [types, languages, searchIndexes] = await Promise.all(
    [
      {
        field: 'types',
        def: { idSize: 0 }
      },
      {
        field: 'languages',
        def: []
      },
      {
        field: 'searchIndexes',
        def: {}
      }
    ].map(async ({ field, def }) => {
      const result = await client.redis.hget('schema', field)
      return result === null ? def : JSON.parse(result)
    })
  )

  const schema: Schema = {
    languages,
    types: {}
  }

  const schemas = await client.redis.hgetall('types')

  if (schemas) {
    for (const type in schemas) {
      schema.types[type] = JSON.parse(schemas[type])
    }
  }
  return { types, schema, searchIndexes }
}

export { getSchema, GetSchemaResult }
