import { GetResult } from '~selva/get/types'
import { Id } from '~selva/schema/index'
import getByType from './getByType'
import { TypeSchema } from '../../../src/schema/index'
import * as logger from '../logger'
import { setNestedResult, getNestedField } from './nestedFields'
import { ensureArray } from 'lua/src/util'

function resolveVariable(
  id: Id,
  schemas: Record<string, TypeSchema>,
  variable: string,
  language?: string,
  version?: string
): string {
  const intermediateResult: object = {}
  getByType(intermediateResult, schemas, id, variable, language, version)
  return getNestedField(intermediateResult, variable)
}

function resolveVariables(
  id: Id,
  schemas: Record<string, TypeSchema>,
  fieldDefinition: string,
  language?: string,
  version?: string
): string {
  let str = ''

  let inVariableDef = false
  let variable = ''
  for (let i = 0; i < fieldDefinition.length; i++) {
    if (!inVariableDef && fieldDefinition[i] === '$') {
      inVariableDef = true
    } else if (fieldDefinition[i] === '{' && inVariableDef) {
      // skip
    } else if (inVariableDef && fieldDefinition[i] === '}') {
      str += resolveVariable(id, schemas, variable, language, version)
      variable = ''
      inVariableDef = false
    } else if (inVariableDef) {
      variable += fieldDefinition[i]
    } else {
      str += fieldDefinition[i]
    }
  }

  return str
}

function resolveAll(
  id: Id,
  schemas: Record<string, TypeSchema>,
  fieldAry: string[],
  language?: string,
  version?: string
): string[] {
  const result: string[] = []
  for (let i = 0; i < fieldAry.length; i++) {
    result[i] = resolveVariables(id, schemas, fieldAry[i], language, version)
  }

  return result
}

export default function getWithField(
  result: GetResult,
  schemas: Record<string, TypeSchema>,
  id: Id,
  field: string,
  $field: string | string[],
  language?: string,
  version?: string
): boolean {
  const fieldDefinitions: string[] = resolveAll(
    id,
    schemas,
    ensureArray($field),
    language,
    version
  )

  const intermediateResult: object = {}
  let fromNested: any
  for (const fieldDefinition of fieldDefinitions) {
    if (
      getByType(
        intermediateResult,
        schemas,
        id,
        fieldDefinition,
        language,
        version
      )
    ) {
      fromNested = getNestedField(intermediateResult, fieldDefinition)
      if (fromNested) {
        break
      }
    }
  }

  if (!fromNested) {
    return false
  }

  setNestedResult(result, field, fromNested)
  return true
}
