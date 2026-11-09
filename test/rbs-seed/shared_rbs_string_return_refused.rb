# A method an --rbs signature declares `-> String` whose tail unboxes
# somewhere other than its last expression (shared_rbs_string_return):
# - a `return` inside a begin with an ensure (the value waits in a result
#   slot until the ensure has run), also a def's or a block's own ensure.
# These shapes were refused while a String slot could hold its bytes, since
# answering the box's bytes would hand on a copy a change in place never
# reaches. Every String slot is now the handle, so they compile and answer
# the handle; the check guards that they keep compiling. (An `||` or `&&`
# tail whose operand is a call answering a String is boxed from the handle
# the call publishes: shared_rbs_logic_tail_pickup.)
# spinel: rbs-seed-check
module ShrsrfConf
  class << self
    attr_accessor :key
  end
end

class ShrsrfApp
  def initialize(o)
    @o = o
  end

  def o = @o

  def ens_after(f)
    k = ShrsrfConf.key
    begin
      return k if f
    ensure
      @n = 1
    end
    k
  end

  def ens_reader(f)
    k = ShrsrfConf.key
    begin
      o
      return k if f
    ensure
      @n = 1
    end
    k
  end

  def ens_tail(f)
    k = ShrsrfConf.key
    begin
      return k if f
      "no"
    ensure
      @n = 1
    end
  end

  def ens_def(f)
    k = ShrsrfConf.key
    o
    return k if f
    "x"
  ensure
    @n = 1
  end

  def ens_block
    k = ShrsrfConf.key
    [1].each do
      o
      return k
    ensure
      @n = 2
    end
    "y"
  end
end

def shrsrf_pass(x) = x

buf = +"ab"
ShrsrfConf.key = buf
a = ShrsrfApp.new(+"other")
r = [shrsrf_pass(a.ens_after(true)), shrsrf_pass(a.ens_reader(true)), shrsrf_pass(a.ens_tail(true)),
     shrsrf_pass(a.ens_def(true)), shrsrf_pass(a.ens_block)]
buf << "!"
p r, r.map { |x| x.equal?(buf) }
