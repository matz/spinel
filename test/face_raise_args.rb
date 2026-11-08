# A call with no argument on a boxed receiver that the face's kind does not
# accept (a Hash, Array, String or Enumerable face) raises NoMethodError whose
# args are the empty list, as the unresolved call's does: the receiver is nil,
# an Integer, a Symbol or a collection of another kind. The accepted receivers
# answer as before, and a call with arguments is unchanged.
def t(l)
  r = yield
  puts [l, r].inspect
rescue Exception => e
  puts [l, e.class, (e.args rescue :noargs)].inspect
end
def mk(i) = [nil, {a: 1, b: 2}, [3, 1, 2], +"ab c", 5, :sym][i]
t(:p01_nil) { mk(0).merge! }
t(:p01_ary) { mk(2).merge! }
t(:p01_str) { mk(3).update }
t(:p01_int) { mk(4).fetch_values }
t(:p01_sym) { mk(5).rehash }
t(:p01_ok) { mk(1).merge! }
t(:p02_nil) { mk(0).sort! }
t(:p02_hash) { mk(1).uniq! }
t(:p02_str) { mk(3).shuffle! }
t(:p02_int) { mk(4).flatten! }
t(:p02_ok) { mk(2).sort! }
t(:p03_nil) { mk(0).squeeze }
t(:p03_ary) { mk(2).hex }
t(:p03_int) { mk(4).oct }
t(:p03_sym) { mk(5).squeeze }
t(:p03_ok) { mk(3).hex }
t(:p04_compact_nil) { mk(0).compact! }
t(:p04_compact_int) { mk(4).compact! }
t(:p04_reverse_nil) { mk(0).reverse! }
t(:p04_reverse_sym) { mk(5).reverse! }
t(:p04_fetch_values_nil) { mk(0).fetch_values }
t(:p04_ok_h) { mk(1).compact! }
t(:p04_ok_a) { mk(2).reverse! }
t(:p05_int) { mk(4).product }
t(:p05_ok_a) { mk(2).minmax }
t(:p05_ok_h) { mk(1).minmax }
t(:p07_nil) { mk(0)&.merge! }
t(:p07_int) { mk(4)&.merge! }
t(:p07_ok) { mk(1)&.merge! }
t(:p08_nil) { mk(0).sort_by! { |x| x } }
t(:p08_int) { mk(4).keep_if { |x| x } }
t(:p08_ok) { mk(2).keep_if { |x| x > 1 } }
$n = 0
def rd = (($n += 1); mk(0))
t(:p09_count) { rd.merge! }
p $n
def rd2 = (($n += 1); mk(1))
t(:p09_ok) { rd2.merge! }
p $n
# The Enumerable face reads its receiver once too, accepted or rejected.
def rc(i) = (($n += 1); mk(i))
t(:p12_nil) { rc(0).product }
t(:p12_int) { rc(4).product }
t(:p12_ok) { rc(2).product }
p $n
