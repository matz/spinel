# A send whose name is known only at run time emits an arm per candidate
# method, each under a probe that drops an arm it cannot emit. Zone#parse's
# default argument is refused, so its arm is dropped -- after the call had
# set self to the boxed receiver for spelling that default. Left so, the
# next arms read the receiver's ivars through a stale self and the C did
# not compile.
class Zone
  def initialize(n) = @n = n
  def parse(s, at = proc { "#{@n}" }.ruby2_keywords) = "#{s}@#{at}"
  def zname(x) = "#{@n}#{x}"
end

class W
  def initialize(o) = @o = o
  def fwd(m, x) = @o.__send__(m, x)
end

zw = W.new(Zone.new("z"))
sw = W.new("str")
p zw.fwd(:zname, 1)
p sw.fwd(:center, 7)
p(sw.fwd(:zname, 2)) rescue p $!.class
# names parse among the candidates; never run, as its arm is dropped
p zw.fwd(:parse, "x") if ARGV.size > 99
