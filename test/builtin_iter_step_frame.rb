# A block given to sum, a seeded inject, sort_by, uniq, a comparator for
# sort, min or max, bsearch or gsub: `next v` answers v for that step and
# moves on, a block local is fresh on every step, and `redo` re-runs the
# step with the parameters it was given and the locals it wrote.

a = [3, 1]
p(a.sum { |q| next -5 if q == 3; 1 })
p(a.sum(0.0) { |q| next 0.5 if q == 3; q })
p([3, "s"].sum(0) { |q| next 10 if q == 3; 1 })
p(a.inject(0) { |s, q| next s + 10 if q == 3; s + q })
p([3, 1, 2].sort_by { |q| next -q if q > 1; q })
p([3, 1, 2].dup.sort_by! { |q| next -q if q > 1; q })
p([3, 1].uniq { |q| next 1 if q == 3; 1 })
p([3, 1, 2].sort { |x, y| next y <=> x if x; 0 })
p([3, 1, 2].min { |x, y| next y <=> x if x; 0 })
p([3, 1, 2].max { |x, y| next x <=> y if x; 0 })
p((1..10).bsearch { |x| next x >= 4 if x; false })
p([1, 2, 3].bsearch { |x| next x >= 2 if x; false })
p([1.0, 2.0, 3.0].bsearch { |x| next x >= 2.0 if x; false })
p([1, 2, 3].bsearch_index { |x| next x >= 2 if x; false })
p("abc".gsub(/b/) { |m| next "X" if m == "b"; m })
p("abc".sub(/[ab]/) { |m| next "Y" if m == "a"; "z" })

# fresh locals
$o = []
p(a.sum { |q| z = (z || 0) + 1; z })
p(a.inject(0) { |s, q| z = (z || 0) + 1; s + z })
[3, 1].sort_by { |q| z = (z || 0) + 1; $o << z; q }
[3, 1].uniq { |q| z = (z || 0) + 1; $o << z; q }
[3, 1, 2].sort { |x, y| z = (z || 0) + 1; $o << z; x <=> y }
"ab".gsub(/./) { |m| z = (z || 0) + 1; $o << z; m }
p $o.uniq

# redo
d = false
p(a.sum { |q| t = (t || 0) + 1; unless d; d = true; redo; end; t })
d = false
p(a.inject(0) { |s, q| t = (t || 0) + 1; unless d; d = true; redo; end; s + t })
d = false
p([3, 1].sort_by { |q| t = (t || 0) + 1; unless d; d = true; redo; end; q * t })
d = false
p("ab".gsub(/./) { |m| t = (t || 0) + 1; unless d; d = true; redo; end; m * t })
