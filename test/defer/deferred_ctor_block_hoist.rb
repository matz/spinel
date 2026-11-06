# Compiled with --defer-refusals: `k.new(arg) { }` on a class value hoists
# its block ahead of the arguments, and a refused argument drops the
# statement, block and all. A later constructor whose initialize keeps its
# block (C.new, no block given) passed the dropped block's temporary, which
# the C no longer declared. Without the flag it is refused.
# spinel: defer-refusals: 2:refused 2 after
class A
  def initialize(x, &b) = (@x = x; @b = b)
  def r = @b ? @b.call(@x) : @x
end
class B
  def initialize(x, &b) = (@x = x; @b = b)
  def r = @b ? @b.call(@x) : 0
end
class C
  def initialize(&b) = @b = b
  def r = @b ? 1 : 2
end
k = ARGV.size > 5 ? B : A
begin
  o = k.new(eval("1")) { |v| v + 1 }
  p o.r
rescue NotImplementedError
  puts "refused"
end
p C.new.r
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
