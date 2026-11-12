# A boxed value that may be a class whose own singleton `send` answers, or an
# instance of a class whose `send` does, or neither: each value takes the send
# that is its own, and the ones no send of the program's answers run the named
# method (Object#send), however many owners the program has on either side.

class K
  def send(*a) = [:isend, *a]
  def self.send(*a) = [:ksend, *a]
  def m = :im
  def self.m = :km
end
x = [K, K.new][ARGV.size]
p x.send(:m)
y = [K.new, K][ARGV.size]
p y.send(:m)

module E
  def send(*a) = [:esend, *a]
end
class Ext
  extend E
  def self.m = :em
end
class Plain
  def m = :pm
end
p Ext.send(:m)
[Ext, Plain.new, 3].each { |o| p((o.send(:m) rescue $!.class)) }

class P
  def self.send(*a) = [:psend, *a]
end
class Sub < P
  def self.m = :subm
end
[Sub, P, Plain.new].each { |o| p((o.send(:m) rescue $!.class)) }

# a value of no class the program names: the owners are asked for each
def pick(i) = [K, Plain.new, "s", 3, nil, Sub][i]
p pick(0).send(:m)
p pick(1).send(:m)
p pick(5).send(:m)

# in a class's own body self is the class
class Body
  def self.send(*a) = [:bsend, *a]
  def self.m = :bm
  p send(:m)
end
