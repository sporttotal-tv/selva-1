import { GetOptions } from '../types'
import { SelvaClient } from '../..'

import checkAllowed from './checkAllowed'

import validateField from './field'
import validateInherit from './inherit'
import validateList from './list'
import validateFind from './find'

function validateNested(
  client: SelvaClient,
  props: GetOptions | true,
  path: string
): void {
  if (props === true) {
    return
  }

  if (props.$id || props.$alias) {
    return validateTopLevel(client, props, path)
  }

  for (const field in props) {
    if (field.startsWith('$')) {
      if (field === '$field') {
        validateField(client, props.$field, path)
      } else if (field === '$inherit') {
        validateInherit(client, props.$inherit, path)
      } else if (field === '$list') {
        validateList(client, props.$list, path)
      } else if (field === '$find') {
        validateFind(client, props.$find, path)
      } else if (field === '$default') {
        continue
      } else if (field === '$all') {
        if (typeof props.$all !== 'boolean') {
          throw new Error(
            `Operator $all for ${path}.$all must be a boolean, got ${props.$all}`
          )
        }
      } else if (field === '$value') {
        // basically anything is allowed in $value
        continue
      } else {
        throw new Error(
          `Operator ${field} is not supported in nested fields for ${path +
            '.' +
            field}`
        )
      }
    }
  }

  for (const field in props) {
    if (!field.startsWith('$')) {
      validateNested(client, props[field], path + '.' + field)
    }
  }
}

export default function validateTopLevel(
  client: SelvaClient,
  props: GetOptions,
  path: string = ''
): void {
  for (const field in props) {
    if (field.startsWith('$')) {
      if (field === '$id') {
        if (typeof props.$id !== 'string' && !Array.isArray(props.$id)) {
          if (path !== '' && typeof props.$id === 'object') {
            const allowed = checkAllowed(props.$id, new Set(['$field']))
            if (allowed !== true) {
              throw new Error(
                `${path}.$id is an object and with unallowed field ${allowed}, only $field is allowed in $id of nested ueries`
              )
            }

            continue
          }

          if (path !== '') {
            throw new Error(
              `$id ${props.$id} in a nested query should be a string, an array of strings or an object with $field reference`
            )
          } else {
            throw new Error(
              `$id ${props.$id} should be a string or an array of strings`
            )
          }
        }
      } else if (field === '$includeMeta') {
        // internal option
        continue
      } else if (field === '$alias') {
        if (typeof props.$alias !== 'string' && !Array.isArray(props.$alias)) {
          throw new Error(
            `${path}.$alias ${props.$alias} should be a string or an array of strings`
          )
        }
      } else if (field === '$version') {
        if (typeof props.$id !== 'string') {
          throw new Error(`$version should be a string`)
        }
      } else if (field === '$language') {
        if (client.schema) {
          if (
            typeof props.$language !== 'string' ||
            !client.schema.languages ||
            !client.schema.languages.includes(props.$language)
          ) {
            throw new Error(
              `$language ${
                props.$language
              } is unsupported, should be a string and one of: ${client.schema.languages.join(
                ', '
              )}`
            )
          }
        }
      } else if (field === '$rawAncestors') {
        if (typeof props.$rawAncestors !== 'boolean') {
          throw new Error(`$rawAncestors should be a boolean value`)
        }
      } else if (field === '$all') {
        if (typeof props.$all !== 'boolean') {
          throw new Error(
            `Operator $all for ${path}.$all must be a boolean, got ${props.$all}`
          )
        }
      } else {
        throw new Error(`
          Top level query operator ${field} is not supported. Did you mean one of the following supported top level query options?
            - $id
            - $alias
            - $all
            - $version
            - $language
          `)
      }
    }
  }

  for (const field in props) {
    if (!field.startsWith('$')) {
      validateNested(client, props[field], path + '.' + field)
    }
  }
}
