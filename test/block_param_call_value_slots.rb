# A `blk.call` of a method's own `&blk` is spliced as a yield is, and its
# value goes into the slot of whatever reads it. Four shapes the yield form
# already built did not build in the `blk.call` form:

# A block whose tail is a call that answers no value (a super with nothing
# above it) or one no class answers (`nil.at(0)`), into a boxed slot: the C
# value of the call is a placeholder, a `const char *` or a 0.
class Top < BasicObject
  def zed = super
end
def try(&blk)
  r = blk.call
  puts "#{r.inspect}"
rescue NoMethodError => e
  puts e.class
end
try { Top.new.zed }
try { "x" }
try { nil.at(0) }

def show(&blk)
  p blk.call
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end
show { nil.at(0) }

# A parenthesized call whose block answers nil at one site and an Integer at
# another: the parentheses are boxed, and the nil one was a bare 0.
def paren(&blk)
  r = (blk.call) rescue 7
  p r
end
paren { nil }
paren { 5 }
paren { raise "no" }

def paren_seq(&blk)
  r = (puts :ran; blk.call) rescue (1..2)
  p r
end
paren_seq { nil }
paren_seq { "s" }
paren_seq { raise "no" }

# A rescue modifier whose fallback is an Array, around a call whose block
# answers nil: the slot keeps its nil, as for a yield.
class Holder
  def initialize; @a = [1, 2, 3]; end
  def run(&blk) = (blk.call(0) rescue @a)
end
p Holder.new.run { |x| nil }
p Holder.new.run { |x| raise "z" }
