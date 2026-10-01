# The in-place filters, map!, fill, product, tap, then, map.with_index,
# each_cons(n).map and the String and index walks: `next v` answers v for
# that step, a block local is fresh on every step, and `redo` re-runs the
# step with the parameters it was given and the locals it wrote.

a = [3, 1, 2]
p(a.dup.map! { |q| next -q if q == 3; q })
p(a.dup.fill { |i| next 9 if i == 0; i })
p(a.map.with_index { |q, i| next 0 if i == 0; q + i })
p(a.each_cons(2).map { |w| next [] if w[0] == 3; w })
p(5.then { |q| next "s" if q > 1; q })
p(a.then { |q| next 7 if q.size > 1; nil })
p(a.select.with_index { |q, i| next true if i == 0; false })

# fresh locals
$o = []
a.dup.select! { |q| z = (z || 0) + 1; $o << z; true }
a.dup.keep_if { |q| z = (z || 0) + 1; $o << z; true }
a.dup.delete_if { |q| z = (z || 0) + 1; $o << z; false }
a.dup.reject! { |q| z = (z || 0) + 1; $o << z; false }
a.dup.map! { |q| z = (z || 0) + 1; $o << z; q }
a.dup.fill { |i| z = (z || 0) + 1; $o << z; i }
a.product([7]) { |t| z = (z || 0) + 1; $o << z }
a.map.with_index { |q, i| z = (z || 0) + 1; $o << z; q }
a.each_cons(2).map { |w| z = (z || 0) + 1; $o << z; w }
a.index { |q| z = (z || 0) + 1; $o << z; false }
p $o.uniq

# redo
def run
  d = false
  yield(-> { r = d; d = true; r })
end
run { |once| p([3, 1].dup.select! { |q| t = (t || 0) + 1; redo unless once.(); t == 2 }) }
run { |once| p([3, 1].dup.map! { |q| t = (t || 0) + 1; redo unless once.(); q * t }) }
run { |once| p([3, 1].dup.fill { |i| t = (t || 0) + 1; redo unless once.(); i * t }) }
run { |once| p([3, 1].map.with_index { |q, i| t = (t || 0) + 1; redo unless once.(); q * t }) }
run { |once| $o = []; [3, 1].product([7]) { |x| t = (t || 0) + 1; redo unless once.(); $o << t }; p $o }
run { |once| $o = []; [3, 1].tap { |x| t = (t || 0) + 1; redo unless once.(); $o << t }; p $o }
run { |once| $o = []; "ab".each_char { |c| t = (t || 0) + 1; redo unless once.(); $o << t }; p $o }
run { |once| $o = []; [3, 1].each_index { |i| t = (t || 0) + 1; redo unless once.(); $o << t }; p $o }
