# A builtin exception whose own initialize reads more than the message (NameError's name,
# NoMethodError's args, KeyError's and FrozenError's key:/receiver:, SystemExit's status) is
# built natively in Spinel. CRuby's initialize of those reads its arguments and calls super
# with the message alone, so a program's reopening of StandardError#initialize runs on that
# message, and the name, key, receiver and status the call gave are still recorded.
class StandardError
  def initialize(msg = nil, &blk)
    @seen = msg
    super("S[#{msg}]")
  end
  attr_reader :seen
end

n = NameError.new("n", :field)
p n.message, n.name
n = NameError.new("n", :f, receiver: 5)
p n.message, n.name, n.receiver
n = NameError.new("n")
p n.message
n = NameError.new
p n.message
m = NoMethodError.new("m", :meth, [1, 2])
p m.message, m.name, m.args
m = NoMethodError.new("m", :meth, [1], receiver: 7)
p m.message, m.receiver
k = KeyError.new("k", receiver: 1, key: :kk)
p k.message, k.key, k.receiver
k = KeyError.new("k")
p k.message
k = KeyError.new
p k.message
f = FrozenError.new("f", receiver: "r")
p f.message, f.receiver
begin; raise NameError.new("rn", :zz); rescue => e; p e.message, e.name; end
begin; raise KeyError.new("rk", key: 1, receiver: {}); rescue => e; p e.message, e.key; end
begin; raise NameError, "x"; rescue => e; p e.message; end
begin; raise KeyError, "y"; rescue => e; p e.message; end

x = SystemExit.new(3, "bye")
p x.message, x.status
x = SystemExit.new("only")
p x.message
x = SignalException.new(2, "custom")
p x.message
x = Interrupt.new("stop")
p x.message
args = ["sp", :nm]
x = NameError.new(*args)
p x.message, x.name, x.seen
begin; raise SystemExit.new(1, "se"); rescue SystemExit => e; p e.message, e.status; end
begin; raise NoMethodError.new("nme", :zz, [1]); rescue => e; p e.message, e.args; end
begin; raise FrozenError.new("fz", receiver: [1]); rescue => e; p e.message, e.receiver; end
def mk(a, b) = NameError.new(a, b)
p mk("dyn", :d).message
p [NameError.new("l1", :a), KeyError.new("l2", key: 1)].map(&:message)
c = NameError
p c.new("cv", :n).message

# the errors the runtime raises are untouched: CRuby builds these without the reopening
begin; {a: 1}.fetch(:b); rescue => e; p e.message; end
begin; 1.zork; rescue NoMethodError => e; p e.message; end
