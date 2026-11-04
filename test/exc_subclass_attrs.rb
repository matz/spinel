# A class below KeyError, NameError, NoMethodError or FrozenError takes what its
# parent's initialize takes: the key: and receiver: keywords, the name, the args
# and the private flag, by `new`, by a class value's `new` and by `super`. A class
# below any other exception takes one message. Their readers, and StopIteration's
# result, answer on an instance of such a class and on a boxed value.
def t(label)
  v = yield
  puts "#{label}: #{v.inspect}"
rescue Exception => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class K < KeyError; end
class N < NameError; end
class M < NoMethodError; end
class F < FrozenError; end
class S < StopIteration; end
class Mine < StandardError; end
class KK < K; end
class WithIvar < NameError
  attr_accessor :w
end
Anon = Class.new(KeyError)
AnonN = Class.new(NameError) { def hi = "hi" }

# new, every argument shape
t("K bare") { x = K.new; [x.message, (x.key rescue $!.message), (x.receiver rescue $!.message)] }
t("K nil") { K.new(nil).message }
t("K two") { K.new("m", 1) }
t("K both") { x = K.new("m", key: :a, receiver: {b: 1}); [x.message, x.key, x.receiver] }
t("K key") { x = K.new("m", key: :a); [x.key, (x.receiver rescue $!.message)] }
t("K receiver") { x = K.new("m", receiver: 1); [x.receiver, (x.key rescue $!.message)] }
t("K alone") { x = K.new(key: :a); [x.message, x.key] }
t("K nils") { x = K.new("m", key: nil, receiver: nil); [x.key, x.receiver] }
t("K unknown") { K.new("m", foo: 1) }
t("K unknowns") { K.new("m", key: 1, foo: 1, bar: 2) }
t("K string key") { K.new("m", "key" => 1) }
t("K hash") { x = K.new({key: 1}); [x.message, (x.key rescue $!.message)] }
t("K spread") { h = {key: 1}; K.new("m", **h).key }
t("K spread mix") { h = {receiver: 2}; x = K.new("m", key: 1, **h); [x.key, x.receiver] }
t("K spread none") { h = {}; x = K.new("m", **h); [x.message, (x.key rescue $!.message)] }
t("K messages") { [K.new(:sym, key: 1).message, K.new(7, key: 1).message, K.new(nil, key: 1).message, K.new("", key: 1).message] }
t("K order") { x = K.new((puts "a"; "m"), key: (puts "b"; 1), receiver: (puts "c"; 2)); [x.message, x.key, x.receiver] }
t("K splat") { a = ["m"]; [K.new(*a).message, K.new(*a, key: 3).key] }
t("K splat none") { a = []; K.new(*a).message }
t("K splat two") { a = ["m", 1]; K.new(*a) }
t("KK") { x = KK.new("m", key: 2, receiver: 3); [x.class, x.key, x.receiver] }
t("N") { x = N.new; [x.message, x.name, (x.receiver rescue $!.message)] }
t("N name") { x = N.new("m", :f); [x.message, x.name, (x.receiver rescue $!.message)] }
t("N string") { N.new("m", "f").name }
t("N nil") { N.new("m", nil).name }
t("N three") { N.new("m", :a, :b) }
t("N receiver") { x = N.new("m", receiver: 5); [x.name, x.receiver] }
t("N both") { x = N.new("m", :f, receiver: nil); [x.name, x.receiver] }
t("N alone") { x = N.new(receiver: 5); [x.message, x.name, x.receiver] }
t("N unknown") { N.new("m", key: 5) }
t("N splat") { a = ["m", :x]; x = N.new(*a); [x.message, x.name] }
t("M") { x = M.new("m"); [x.message, x.name, x.args, x.private_call?] }
t("M name") { x = M.new("m", :f); [x.name, x.args, x.private_call?, (x.receiver rescue $!.message)] }
t("M args") { x = M.new("m", :f, [1, 2]); [x.name, x.args, x.private_call?] }
t("M args others") { [M.new("m", :f, nil).args, M.new("m", :f, 3).args] }
t("M private") { [M.new("m", :f, [1], true).private_call?, M.new("m", :f, [1], false).private_call?, M.new("m", :f, [1], 7).private_call?] }
t("M five") { M.new("m", :f, [1], true, 2) }
t("M receiver") { x = M.new("m", :f, [1], true, receiver: 9); [x.receiver, x.args] }
t("M unknown") { M.new("m", :f, foo: 1) }
t("F") { x = F.new; [x.message, (x.receiver rescue $!.message)] }
t("F receiver") { x = F.new("m", receiver: 1); [x.message, x.receiver] }
t("F nil") { F.new("m", receiver: nil).receiver }
t("F alone") { x = F.new(receiver: 1); [x.message, x.receiver] }
t("F two") { F.new("m", 1) }
t("F unknown") { F.new("m", key: 1) }
t("S") { [S.new.message, S.new.result, S.new("m").message, S.new("m").result] }
t("S two") { S.new("m", 1) }
t("S keyword") { S.new("m", result: 1) }
t("Mine") { [Mine.new("a", "b")] }
t("Mine three") { Mine.new("a", "b", "c") }
t("Mine keyword") { Mine.new("a", b: 1) }
t("Mine order") { Mine.new((puts "x"; "a"), b: (puts "y"; 1)) }
t("Mine alone") { Mine.new(b: 1).message }
t("Mine result") { Mine.new.result }
t("result off StopIteration") { RuntimeError.new.result }
t("ivar") { x = WithIvar.new("m", :n, receiver: 4); x.w = [1]; [x.name, x.receiver, x.w, x.message] }
t("anonymous") { x = Anon.new("m", key: 2); [x.key, x.message] }
t("anonymous name") { x = AnonN.new("m", :z); [x.name, x.hi] }
t("dup") { x = K.new("m", key: 1, receiver: 2).dup; [x.key, x.receiver] }
t("copy") { x = K.new("m", key: 1).exception("n"); [x.message, x.key] }

# raise and rescue
t("raise") { begin; raise K, "x"; rescue K => e; [e.message, (e.key rescue $!.message), (e.receiver rescue $!.message)]; end }
t("raise N") { begin; raise N, "x"; rescue N => e; [e.message, e.name, (e.receiver rescue $!.message)]; end }
t("raise new") { begin; raise K.new("x", key: 1); rescue KeyError => e; [e.class, e.key]; end }
t("rescue parent") { begin; raise N.new("x", :q); rescue NameError => e; [e.class, e.name]; end }
t("raise M") { begin; raise M.new("m", :f, [1, 2], true, receiver: "r"); rescue NoMethodError => e; [e.class, e.message, e.name, e.args, e.private_call?, e.receiver]; end }

# super, with arguments and bare
class SK < KeyError
  attr_reader :extra
  def initialize(msg, k, extra = 7)
    super(msg, key: k, receiver: [k])
    @extra = extra
  end
end
class SKBare < KeyError
  def initialize(msg = "dflt", key: :dk, receiver: nil)
    super
  end
end
class SKRest < KeyError
  def initialize(*a, **kw)
    super
  end
end
class SKSpread < KeyError
  def initialize(msg, **kw)
    super(msg, **kw)
  end
end
class SKNone < KeyError
  def initialize; end
end
class SKMsg < KeyError
  def initialize
    super("six")
  end
end
class SKOpt < KeyError
  def initialize(m, opt)
    super(m, **opt)
  end
end
class SKBad < KeyError
  def initialize; super("m", 1); end
end
class SKUnknown < KeyError
  def initialize; super("m", foo: 1); end
end
class SN < NameError
  def initialize(n)
    super("bad #{n}", n)
  end
end
class SNBare < NameError
  def initialize(msg, name)
    super
  end
end
class SNRecv < NameError
  def initialize(n)
    super("r", n, receiver: self.class)
  end
end
class SM < NoMethodError
  def initialize(n)
    super("bad", n, [n], true, receiver: 3)
  end
end
class SF < FrozenError
  def initialize(o)
    super("frozen #{o}", receiver: o)
  end
end
class SG < SF
  def initialize(o)
    super(o.to_s * 2)
  end
end
class SFBad < FrozenError
  def initialize; super("m", key: 1); end
end
class SS < StopIteration
  def initialize(m); super(m); end
end
class Top < KeyError
  def initialize(m); super(m, key: 1); end
end
class Below < Top
  def initialize; super("b"); end
end
class Plain < Top; end
class OwnKey < KeyError
  attr_reader :key
  def initialize(m, k); @key = k * 2; super(m, key: k); end
end
class Maker < KeyError
  def self.make(k) = new("made", key: k)
  def self.make_self(k) = self.new("self", key: k)
end
class NameMaker < NameError
  def self.make(n) = new("made", n)
end
t("SK") { x = SK.new("m", :a); [x.message, x.key, x.receiver, x.extra] }
t("SKBare") { x = SKBare.new; [x.message, x.key, x.receiver] }
t("SKBare given") { x = SKBare.new("q", key: 1, receiver: 2); [x.message, x.key, x.receiver] }
t("SKRest") { x = SKRest.new("q", key: 1, receiver: 2); [x.message, x.key, x.receiver] }
t("SKSpread") { x = SKSpread.new("q", key: 1); [x.message, x.key, (x.receiver rescue $!.message)] }
t("SKNone") { x = SKNone.new; [x.message, (x.key rescue $!.message)] }
t("SKMsg") { x = SKMsg.new; [x.message, (x.key rescue $!.message)] }
t("SKOpt") { x = SKOpt.new("q", {key: 3}); [x.message, x.key] }
t("SKBad") { SKBad.new }
t("SKUnknown") { SKUnknown.new }
t("SN") { x = SN.new(:z); [x.message, x.name, (x.receiver rescue $!.message)] }
t("SNBare") { x = SNBare.new("q", :y); [x.message, x.name] }
t("SNRecv") { x = SNRecv.new(:y); [x.message, x.name, x.receiver] }
t("SM") { x = SM.new(:y); [x.message, x.name, x.args, x.private_call?, x.receiver] }
t("SF") { x = SF.new("o"); [x.message, x.receiver] }
t("SG") { x = SG.new("o"); [x.message, x.receiver] }
t("SFBad") { SFBad.new }
t("SS") { x = SS.new("m"); [x.message, x.result] }
t("Below") { x = Below.new; [x.message, x.key] }
t("Plain") { x = Plain.new("c"); [x.class, x.message, x.key] }
t("OwnKey") { x = OwnKey.new("m", 2); [x.key, x.message] }
t("Maker") { x = Maker.make(1); [x.message, x.key] }
t("Maker self") { x = Maker.make_self(1); [x.message, x.key] }
t("NameMaker") { x = NameMaker.make(:p); [x.message, x.name] }

# a class in a variable
kv = [K, N][0]
nv = [N, K][0]
mv = [M, K][0]
fv = [F, K][0]
sv = [S, K][0]
t("value K") { x = kv.new("m"); [x.message, (x.key rescue $!.message)] }
t("value K keyword") { x = kv.new("m", key: :v); [x.message, x.key] }
t("value N") { x = nv.new("m", :v); [x.message, x.name] }
t("value N keyword") { x = nv.new("m", :v, receiver: 1); [x.name, x.receiver] }
t("value N splat") { a = ["m", :x]; x = nv.new(*a); [x.message, x.name] }
t("value M") { x = mv.new("m", :v, [1], true); [x.name, x.args, x.private_call?] }
t("value F") { x = fv.new("m", receiver: 8); [x.message, x.receiver] }
t("value S") { x = sv.new("m"); [x.message, x.result] }
t("value raise") { begin; raise kv, "hi"; rescue KeyError => e; [e.class, e.message]; end }

# a boxed receiver: each reader on each class
[K.new("m", key: :a, receiver: 1), N.new("m", :b, receiver: 2), M.new("m", :c, [3], true, receiver: 3),
 F.new("m", receiver: 4), S.new("m"), KeyError.new("m", key: :e), NameError.new("m", :f), NoMethodError.new("m", :g, [1]),
 FrozenError.new("m", receiver: 5), StopIteration.new("m"), RuntimeError.new("r"), "str", 1].each do |v|
  t("key #{v.class}") { v.key }
  t("receiver #{v.class}") { v.receiver }
  t("name #{v.class}") { v.name }
  t("args #{v.class}") { v.args }
  t("private_call? #{v.class}") { v.private_call? }
  t("result #{v.class}") { v.result }
end

# #name on the other boxed values that answer it stays as it is
t("name Symbol") { x = [:abc, 1][0]; [x.name, x.name.frozen?] }
t("name Class") { x = [String, 1][0]; [x.name, x.name.frozen?] }
t("name anonymous") { x = [AnonN, 1][0]; x.name }
t("name Encoding") { x = [Encoding::UTF_8, 1][0]; x.name }
t("name nil") { x = [nil, 1][0]; x.name }
t("name raised") { begin; zork; rescue NameError => e; x = [e, 1][0]; [x.class, x.name]; end }
t("name raised method") { begin; nil.foo; rescue NoMethodError => e; x = [e, 1][0]; [x.name, x.receiver, x.args]; end }
class Named
  attr_reader :name, :key
  def initialize; @name = "n"; @key = "k"; end
end
[Named.new, N.new("m", :q), K.new("m", key: :r)].each do |v|
  t("beside a reader") { [(v.name rescue $!.message), (v.key rescue $!.message)] }
end

# a later super replaces what an earlier one set, and none clears the message
class Twice < KeyError
  def initialize(a, b)
    super(a, key: 1)
    super(b, key: 2)
  end
end
class TwiceKey < KeyError
  def initialize(a)
    super(a, key: 1)
    super(key: 2)
  end
end
class TwiceName < NameError
  def initialize(a)
    super(a, :first, receiver: 1)
    super(a, :second)
  end
end
class TwiceBare < NameError
  def initialize(a)
    super(a, :first)
    super()
  end
end
t("Twice nil") { x = Twice.new("a", nil); [x.message, x.key] }
t("Twice") { x = Twice.new("a", "b"); [x.message, x.key] }
t("TwiceKey") { x = TwiceKey.new("a"); [x.message, x.key] }
t("TwiceName") { x = TwiceName.new("m"); [x.name, x.receiver] }
t("TwiceBare") { x = TwiceBare.new("m"); [x.message, x.name] }

# a String message is the String given, as without the keywords
t("message held") { s = +"abc"; x = K.new(s, key: 1); s << "d"; [x.message, x.message.equal?(s)] }
t("message held name") { s = +"abc"; x = N.new(s, :n); s << "d"; x.message }
t("message held super") { s = +"abc"; x = Twice.new("a", s); s << "d"; x.message }
t("message held plain") { s = +"abc"; x = K.new(s); s << "d"; [x.message, x.message.equal?(s)] }

# a bare identifier in instance_eval on a boxed value is a NameError
[1, "s"].each do |v|
  t("vcall #{v.class}") { v.instance_eval { name } }
end

# what the exception holds survives a collection
3000.times do |i|
  x = K.new("m#{i}", key: "k#{i}", receiver: ["r#{i}"])
  y = N.new("n#{i}", "s#{i}".to_sym, receiver: {a: "v#{i}"})
  z = M.new("z#{i}", :q, ["a#{i}"], true)
  w = WithIvar.new("w#{i}", :w, receiver: "x#{i}")
  w.w = "i#{i}"
  raise "bad" unless x.key == "k#{i}" && x.receiver == ["r#{i}"] && y.receiver[:a] == "v#{i}" && z.args == ["a#{i}"] && w.w == "i#{i}" && w.receiver == "x#{i}" && w.message == "w#{i}"
end
puts "collected"
