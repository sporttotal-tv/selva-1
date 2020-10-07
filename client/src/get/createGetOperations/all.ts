import { SelvaClient } from '../../'
import { GetOptions } from '..'
import { GetOperation } from '../types'
import { getTypeFromId, getNestedSchema } from '../utils'
import createGetOperations from './'
// needs async to fetch schema...
const all = (
  client: SelvaClient,
  props: GetOptions,
  id: string,
  field: string,
  db: string,
  ops: GetOperation[] = []
): GetOperation[] => {
  const schema = client.schemas[db]
  if (field === '') {
    const type = getTypeFromId(schema, id)
    const typeSchema = type === 'root' ? schema.rootType : schema.types[type]

    if (!typeSchema) {
      return
    }

    for (const key in typeSchema.fields) {
      if (
        key !== 'children' &&
        key !== 'parents' &&
        key !== 'ancestors' &&
        key !== 'descendants'
      ) {
        if (props[key] === undefined) {
          ops.push({
            type: 'db',
            id,
            field: key,
            sourceField: key
          })
        } else if (props[key] === false) {
          // do nothing
        } else {
          createGetOperations(
            client,
            props[key],
            id,
            field + '.' + key,
            db,
            ops
          )
        }
      }
    }

    const fieldSchema = getNestedSchema(schema, id, field.slice(1))
    if (!fieldSchema) {
      return
    }

    if (fieldSchema.type === 'object') {
      for (const key in fieldSchema.properties) {
        if (props[key] === undefined) {
          ops.push({
            type: 'db',
            id,
            field: field.slice(1) + '.' + key,
            sourceField: field.slice(1) + '.' + key
          })
        } else if (props[key] === false) {
          // do nothing
        } else {
          createGetOperations(
            client,
            props[key],
            id,
            field + '.' + key,
            db,
            ops
          )
        }
      }
    } else if (fieldSchema.type === 'record' || fieldSchema.type === 'text') {
      // basically this is the same as: `field: true`
      ops.push({
        type: 'db',
        id,
        field: field.slice(1),
        sourceField: field.slice(1)
      })
    }
  }
  return ops
}

export default all