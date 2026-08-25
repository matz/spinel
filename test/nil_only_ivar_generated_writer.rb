# An ivar whose only `@x = ...` in the source is `nil` is treated as statically
# falsy, and `if @x` / `if obj.x` has its live branch dropped at compile time.
# That scan counts InstanceVariableWriteNodes, and a GENERATED writer leaves
# none behind: attr_writer / attr_accessor synthesize the setter, so `obj.x = v`
# is a call against a body with no AST. An object built with `@x = nil` and then
# assigned from outside therefore read as "every write is nil", and the branch
# was dropped -- silently, since `.nil?`, `is_a?` and assigning to a local all
# still emitted a real check and reported the value as present.
#
# The fold itself is wanted (optcarrot's `if @conf.stackprof_mode`), so the last
# two cases pin the boundary the fix draws: a setter whose body is real source
# keeps the branch, and an ivar with no writer at all still folds.
#
# Each case uses a DISTINCT ivar name on purpose. The scan is keyed on the name
# across the whole program, so sharing one name would let the hand-written
# setter's `@x = v` suppress the fold for the other cases and hide the bug.

class ViaAccessor
  attr_accessor :acc
  def initialize
    @acc = nil
  end
end

class ViaWriter
  attr_writer :wrt
  def initialize
    @wrt = nil
  end
  def check
    if @wrt
      "truthy"
    else
      "falsy"
    end
  end
end

class HandWritten
  attr_reader :hnd
  def initialize
    @hnd = nil
  end
  def hnd=(v)
    @hnd = v
  end
end

class NeverSet
  attr_reader :nvr
  def initialize
    @nvr = nil
  end
end

def acc_truthy(o)
  if o.acc
    "truthy"
  else
    "falsy"
  end
end

def hnd_truthy(o)
  if o.hnd
    "truthy"
  else
    "falsy"
  end
end

def nvr_truthy(o)
  if o.nvr
    "truthy"
  else
    "falsy"
  end
end

a = ViaAccessor.new
a.acc = [0.5, 0.25]
puts "accessor, reader call: " + acc_truthy(a)

b = ViaWriter.new
b.wrt = [1.0]
puts "writer, direct ivar:   " + b.check

h = HandWritten.new
h.hnd = [2.0]
puts "hand-written setter:   " + hnd_truthy(h)

puts "no writer at all:      " + nvr_truthy(NeverSet.new)
