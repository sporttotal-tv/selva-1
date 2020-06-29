RPN Filter Expressions
======================

RPN filter expressions allows creating simple expressions for filtering lookups
in Hierarchy. The expressions are pure functions that can have no side effects
of any kind. An expression can only return boolean true or false, or its
execution can fail with an error. The name RPN comes from Reverse Polish
notation, which is the notation used in the expression language. Briefly the
benefit of using this notation is that the expressions don't need parenthesis
and it's very fast to parse because there are no precedence rules.

The following query selects all descendants of a node called `grphnode_1` that are of
type `2X`. It's also possible to write the same expression using a single function
but it wouldn't be as interesting example as the following filter is.

```
SELVA.HIERARCHY.find test descendants "grphnode_1" '$0 b "2X d'
```

Breaking down the filter:

```
$1      [reg ref]   Reads a string value from the register 1.
b       [function]  Extracts the type string from the previous result.
"2X     [operand 0] Is a string representing a node type.
d       [function]  Compares operand 0 with the result of the previous function.
```


Syntax
------

**Numeric literals**

Numeric literals are prefixed with `#`.

**String literals**

A string literal starts with a `"` character.

Strings cannot be quoted and it's advisable to place strings in the registers
given as arguments to the expression parser.

For example, instead of writing

```
SELVA.HIERARCHY.find test descendants "grphnode_1" '"field f "test c'
```

you should consider writing

```
SELVA.HIERARCHY.find test descendants "grphnode_1" '"field f $1 c' "test"
```

**Register integers**

Integers stored in the register can be referenced with an `@` prefix.

**Register string**

Strings stored in the register can be referenced with a `$` prefix.

**Arithmetic operators**

| Operator | Operands           | Description                       | Syntax                    |
|----------|--------------------|-----------------------------------|---------------------------|
| `A`      | `a + b`            | Addition operator.                | `1 2 A => 3`              |
| `B`      | `a - b`            | Subtraction operator.             | `1 2 B => 1`              |
| `C`      | `a / b`            | Division operator.                | `2 4 C => 2`              |
| `D`      | `a * b`            | Multiplication operator.          | `2 2 D => 4`              |
| `E`      | `a % b`            | Remainder operator.               | `4 9 E => 1`              |

**Relational operators**

| Operator | Operands           | Description                       | Example (expr => result)  |
|----------|--------------------|-----------------------------------|---------------------------|
| `F`      | `a == b`           | Equality operator.                | `1 1 F => 1`              |
| `G`      | `a != b`           | Not equal operator.               | `1 2 G => 1`              |
| `H`      | `a < b`            | Less than operator.               | `2 1 H => 1`              |
| `I`      | `a > b`            | Greater than operator.            | `2 1 I => 0`              |
| `J`      | `a <= b`           | Less than or equal operator.      | `2 1 J => 1`              |
| `K`      | `a >= b`           | Greater than or equal operator.   | `2 1 K => 0`              |

**Logical operators**

| Operator | Operands           | Description                       | Example (expr => result)  |
|----------|--------------------|-----------------------------------|---------------------------|
| `L`      | `!a`               | Logical NOT operator. (unary)     | `1 L => 0`                |
| `M`      | `a AND b`          | Logical AND operator.             | `1 1 M => 1`              |
| `N`      | `a OR b`           | Logical OR operator.              | `0 1 N => 1`              |
| `O`      | `!!a XOR !!b`      | Logical XOR operator.             | `1 1 O => 0`              |

**Functions**

| Operator | Arguments          | Description                       | Example (expr => result)  |
|----------|--------------------|-----------------------------------|---------------------------|
| `a`      | `a in b`           | `in` function.                    | `$0 $1 a => 0`            |
| `b`      | `id`               | Returns the type of a node id.    | `xy123 b => xy`           |
| `c`      | `!strcmp(s1, s2)`  | Compare strings.                  | `$0 "hello c => 1`        |
| `d`      | `!cmp(id1, id2)`   | Compare node IDs.                 | `$0 $1 d => 1`            | 
| `e`      | `!cmp(curT, id)`   | Compare the type of the current node. | `"AB e`               |
| `f`      | `node[a]`          | Get the string value of a node field. | `"field f`            |
| `g`      | `node[a]`          | Get the integer value of a node field. | `"field g`           |