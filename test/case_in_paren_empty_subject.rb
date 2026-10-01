# A parenthesized empty `[]` or `{}` as a `case ... in` subject is held in
# its boxed temp as the bare literal is: it went in raw and the C did not
# build.

case([])
in [] then p :empty_array
end
case({})
in {} then p :empty_hash
end
p(case ([])
  in [*rest] then rest
  end)
p(case (({}))
  in {} then true
  end)
