# A send of a name nothing fixes on a class (`self.class` in an instance
# method, self in a class method, a constant) dispatches over the class's
# class methods and Class's own.
class K
  def self.vals = { a: 1 }
  def self.pick(x) = [:vals, :name, :nope][x]
  def self.by_self(x) = send(pick(x))
  def enum_name(x) = x.zero? ? nil : pick_i(x)
  def pick_i(x) = K.pick(x - 1)
  def get(x) = (n = enum_name(x)) ? self.class.send(n) : :none
end
class Sub < K
  def itself(x) = x
end
p K.new.get(0)
p K.new.get(1)
p Sub.new.get(2)
p K.by_self(0)
p Sub.by_self(1)
p K.send(K.pick(1))
begin
  K.new.get(3)
rescue NoMethodError => e
  puts e.message
end
p K.send("it" + "self")
class B2
  def self.who = :b2
end
class A2
  def self.who = :a2
  def run(n) = B2.new.instance_eval { self.class.send(n) }
end
p A2.new.run(ARGV[0] || "who")
