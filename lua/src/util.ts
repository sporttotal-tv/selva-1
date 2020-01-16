export const arrayIsEqual = (a: any[], b: any[]): boolean => {
  const len = a.length
  if (len !== b.length) {
    return false
  }
  for (let i = 0; i < len; i++) {
    if (a[i] !== b[i]) {
      return false
    }
  }
  return true
}

export function isString(val: any): val is string {
  return type(val) === 'string'
}

export function isArray<T>(value: T | T[]): boolean {
  return type(value) === 'table' && (<T[]>value).length > 0 && !!value[0]
}

export function ensureArray<T>(value: T | T[]): T[] {
  if (isArray(value)) {
    return <T[]>value
  }

  return [<T>value]
}

export function splitString(str: string, delim: string): string[] {
  let strings: string[] = []
  let idx = 0
  let current = ''
  for (let i = 0; i < str.length; i++) {
    if (str[i] === delim) {
      strings[idx] = current
      idx++
      current = ''
    }
  }
  strings[idx] = current
  return strings
}

export function joinString(strs: string[], delim: string): string {
  let str = ''
  for (let i = 0; i < strs.length - 1; i++) {
    str += strs[i]
  }

  str += strs[strs.length - 1]
  return str
}
