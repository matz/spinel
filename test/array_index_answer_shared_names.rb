# A typed Array's index answer that is nil calls nil's methods: a name the
# program defines on Integer and on Object (Object's, for nil), `& | ^`
# named by a Symbol in a fold, and `to_s(base)`, which nil's to_s refuses
# (ArgumentError). A found one calls the Integer's.
class Integer
  def label = "int"
end
class Object
  def label = "obj"
end
ia = [3, 1, 4]
miss = ARGV.size == 0 ? 99 : 4
x = ia.index(miss)
p x.label, ia.index(1).label
p [x, 1].reduce(:&), [x, 1].inject(:|), [x, 2].reduce(:^)
p((x.to_s(2) rescue $!.class), ia.index(1).to_s(2))
