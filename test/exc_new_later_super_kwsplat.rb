# NoMethodError#initialize sets the private flag on every call, false when none
# is given. #name off NameError raises with its receiver and empty args, as a
# failed call does. A `**h` passes nothing to the constructor of a class below
# an exception taking one message when it is empty, and one more argument when
# it is not.
def t(label)
  v = yield
  puts "#{label}: #{v.inspect}"
rescue Exception => e
  puts "#{label}: #{e.class}: #{e.message}"
end

class Later < NoMethodError
  def initialize(form)
    super("m", :a, [1], true)
    case form
    when :name then super("m", :a)
    when :message then super("m")
    when :none then super()
    when :args then super("m", :b, [2])
    when :false_flag then super("m", :b, [2], false)
    when :nil_flag then super("m", :b, [2], nil)
    when :true_flag then super("m", :b, [2], true)
    end
  end
end
class Kept < NoMethodError
  def initialize; super("m", :a, [1], true); end
end
class TooMany < NoMethodError
  def initialize
    super("m", :a, [1], true)
    super("m", :a, [1], false, :extra)
  end
end
[:name, :message, :none, :args, :false_flag, :nil_flag, :true_flag].each do |form|
  t("later #{form}") { x = Later.new(form); [x.private_call?, x.args, x.name, x.message] }
end
t("kept") { Kept.new.private_call? }
t("too many") { TooMany.new }

class Own < StandardError; end
class OwnBelow < Own; end
def no_name(v)
  v.name
rescue NoMethodError => e
  [e.class, e.message, e.name, e.args, e.receiver.class, e.receiver.equal?(v)]
end
[RuntimeError.new("r"), Own.new("o"), OwnBelow.new("b"), StopIteration.new("s"), KeyError.new("k", key: 1), SystemExit.new(3)].each do |v|
  t("boxed #{v.class}") { no_name(v) }
end
t("boxed NameError") { no_name(NameError.new("n", :a)) }
t("boxed beside") { x = [RuntimeError.new("r"), 1][0]; no_name(x) }
t("typed") { begin; RuntimeError.new("t").name; rescue NoMethodError => e; [e.message, e.name, e.args, e.receiver.class]; end }
t("typed below") { begin; Own.new("t").name; rescue NoMethodError => e; [e.message, e.name, e.args, e.receiver.class]; end }
t("rescued") { begin; raise "x"; rescue => r; begin; r.name; rescue NoMethodError => e; e.receiver.equal?(r); end; end }

class Mine < StandardError; end
class MineBelow < Mine; end
class WithIvar < StandardError
  attr_accessor :w
end
class Maker < StandardError
  def self.make(o) = new("a", **o)
  def self.pass(**) = new("a", **)
end
empty = {}
full = {k: 1}
t("empty") { Mine.new("a", **empty).message }
t("full") { Mine.new("a", **full) }
t("empty alone") { Mine.new(**empty).message }
t("full alone") { Mine.new(**full).message }
t("string key alone") { h = {"s" => 1}; Mine.new(**h).message }
t("nil message") { Mine.new(nil, **empty).message }
t("empty message") { Mine.new("", **empty).message }
t("below") { MineBelow.new("a", **empty).message }
t("ivar empty") { x = WithIvar.new("a", **empty); x.w = 1; [x.message, x.w] }
t("ivar full") { WithIvar.new("a", **full) }
t("two") { Mine.new("a", "b", **empty) }
t("two full") { Mine.new("a", "b", **full) }
t("both empty") { Mine.new("a", **empty, **empty).message }
t("empty and full") { Mine.new("a", **empty, **full) }
t("literal beside") { Mine.new("a", k: 1, **empty) }
t("splat") { a = ["a"]; Mine.new(*a, **empty).message }
t("splat none") { a = []; Mine.new(*a, **empty).message }
t("splat two") { a = ["a", "b"]; Mine.new(*a, **empty) }
t("splat full") { a = ["a"]; Mine.new(*a, **full) }
t("splat none full") { a = []; Mine.new(*a, **full).message }
t("order") { Mine.new((puts "first"; "a"), **(puts "second"; {})).message }
t("order raises") { Mine.new((puts "first"; "a"), **(puts "second"; {k: 1})) }
t("class method") { Maker.make(empty).message }
t("class method full") { Maker.make(full) }
t("forwarded") { Maker.pass.message }
t("forwarded full") { Maker.pass(k: 1) }
t("held") { s = +"abc"; x = Mine.new(s, **empty); s << "d"; [x.message, x.message.equal?(s)] }
t("raised") { begin; raise Mine.new("a", **empty); rescue Mine => e; e.message; end }

1000.times do |i|
  x = Mine.new("m#{i}", **empty)
  y = Mine.new(**{"k#{i}" => i})
  raise "bad" unless x.message == "m#{i}" && y.message.include?("k#{i}")
end
puts "collected"
