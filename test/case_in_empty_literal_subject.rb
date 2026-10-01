# A `case ... in` subject is held in a temp of its own kind, and an empty
# `[]` or `{}` subject, which has no element type, goes into that temp boxed:
# it went in raw, an Integer array or a String-keyed hash into an sp_RbVal,
# and the C did not build.

p(case []
  in [*everything] then everything
  else false
  end)
p(case [1, 2]
  in [] then :empty
  in [_, *] then :some
  end)
p(case {}
  in {} then true
  end)
p(case {a: 1}
  in {a: Integer => n} then n
  end)
x, y = [3, 4]
p x + y
