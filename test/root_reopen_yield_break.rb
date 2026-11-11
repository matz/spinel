# A block that breaks, given to a yielding method the program defines on
# Object, NilClass or Numeric, answers the break's value.
class Object
  def each_thing; yield 1; 2; end
  def twice; yield 1; yield 2; :done; end
end
class NilClass
  def nil_thing; yield 1; 2; end
end
class Numeric
  def num_thing; yield 1; 2; end
end
r = "a".each_thing { break 9 }
p r
o = Object.new
p(o.each_thing { |v| break v + 8 })
p(5.each_thing { break 7 })
p([1].each_thing { break :a })
p(nil.each_thing { break :n })
# no break taken, and next
p("a".each_thing { |v| v })
p("a".each_thing { next 5 })
# the break is the nested one's, or the outer's
p("a".each_thing { [1, 2, 3].each { |x| break if x == 2 }; 4 })
p("a".each_thing { [1, 2, 3].each { |x| break }; break 6 })
p([1, 2].each { |x| "b".each_thing { break 3 } })
p("a".twice { |v| break "got#{v}" if v == 2 })
p("a".twice { |v| break "got#{v}" if v == 3 })
p(nil.nil_thing { break 9 })
p(nil.nil_thing { |v| v + 1 })
p(3.num_thing { break 9 })
p(3.5.num_thing { break "f" })
p(3.num_thing { |v| v + 1 })
