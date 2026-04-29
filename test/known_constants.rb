# Issue #75: the codegen now rejects an unknown constant reference
# with a "uninitialized constant <Name>" error instead of either
# leaking the bare identifier into the C output (`p Foo`) or
# silently lowering to `0` (`p Foo.bar`). This regression test pins
# the legitimate constant references that look superficially like
# the failing cases but resolve to known names — the fix shouldn't
# reject any of them.

# User-defined class — `find_class_idx` resolves it.
class Box
  def initialize(v); @v = v; end
  attr_reader :v
end

puts Box.new(7).v          # 7

# User-defined constant.
N = 42
puts N                     # 42

# Built-in module-like receiver (Math).
puts Math.sqrt(16).to_i    # 4

# ARGV / STDOUT pass through.
puts ARGV.length           # 0
