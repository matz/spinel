# A binding inside a parenthesized pattern is bound, as without the
# parentheses: in an array element, a hash value, a whole arm, a find
# pattern, an alternation, a one-line `in` and `=>`, and with a guard.

case [1, 2]
in [Integer => a, (Integer => b)] then p [a, b]
end

case [1, [2, 3]]
in [Integer => a, ([Integer => b, *])] then p [a, b]
end

case {a: [1, 2, 3]}
in {a: ([1, *rest])} then p rest
end

case [1, 5, 6]
in ([1, *rest]) then p rest
end

case [7, 8]
in [((Integer => c)), d] then p [c, d]
end

case [3, 4]
in [(Integer => e), f] if e > 2 then p [:guard, e, f]
end

case [:x, 9, :y]
in [*, (Integer => g), *] then p g
end

case {k: "s"}
in {k: ((String | Symbol) => h)} then p h
end

if [10, 11] in [(Integer => i), _]
  p i
end

[12, [13]] => [j, ([k])]
p [j, k]

def pick(v)
  case v
  in {name: (String => n), age: (Integer => m)} then "#{n}:#{m}"
  in [(Symbol => s)] then s.to_s
  else "none"
  end
end
puts pick({name: "ann", age: 3})
puts pick([:sym])
puts pick(5)

lim = 4
[[1, 4], [2, 5]].each do |pair|
  case pair
  in [(Integer => x), ^(lim)] then p [:pin, x]
  in [(Integer => x), y] then p [:other, x, y]
  end
end

case [1, 5, 6]
in ([1, *r]) if r.size > 1 then p r
end

case 5
in (Integer | Float) => n unless n < 0 then p n
end
