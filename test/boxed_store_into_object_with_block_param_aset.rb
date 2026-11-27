# A store into a boxed receiver (`r[k] ||= v`, `r[k] += v`, `r[k] = v` where r
# may be a Hash or an object) reaches the program's `[]=` through the
# runtime's store hook, sp_user_aset_dispatch. A `[]=` that takes a block
# parameter, or the dispatcher of one defined under a condition (whose arms
# take `&__condb`), has a block slot after the value; the hook passed the key
# and the value only, and clang stopped: "too few arguments to function
# call, expected 4, have 3". A large application program's record `[]=`
# is defined in two places under a condition.
class Rec
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v, &blk)
    @h[k] = blk ? blk.call(v) : v
  end
end
r = [Rec.new, {}][ARGV.size % 2]
r[:a] ||= 1
r[:b] = 2
r[:a] += 10
p r[:a], r[:b]

class Attrs
  def initialize = @h = {}
  def [](k) = @h[k]
  if ARGV.size > 3
    def []=(k, v)
      @h[k] = v
    end
  else
    def []=(k, v)
      @h[k] = "#{v}!"
    end
  end
end
a = [Attrs.new, {}][ARGV.size % 2]
a[:x] ||= "v"
a[:y] = 3
p a[:x], a[:y]

# an anonymous `&` parameter, and a `[]=` with none beside them
class AnonBlk
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v, &)
    @h[k] = v
  end
end
class Plain
  def initialize = @h = {}
  def [](k) = @h[k]
  def []=(k, v)
    @h[k] = v
  end
end
[AnonBlk.new, Plain.new, {}].each_with_index do |r, i|
  r = [r, {}][ARGV.size % 2]
  r[:n] ||= i
  r[:n] += 1
  p r[:n]
end
