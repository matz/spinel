# What `exception` builds on a class of Exception answers the class's own
# methods wherever the value goes, as what `new` builds does.
class Stat < StandardError
  attr_reader :n

  def initialize(n = 1)
    super("n=#{n}")
    @n = n
  end

  def twice = @n * 2
  def self.bare(v) = exception(v)
  def self.on_self(v) = self.exception(v)
  def self.by_send(v) = send(:exception, v)
  BUILT = exception(9)
end
class Below < Stat
  def twice = super + 1
end
class Other < StandardError
  attr_reader :m

  def initialize(m = 2)
    super("m=#{m}")
    @m = m
  end
end
# not an exception, with a reader of the same name
class Pair
  attr_reader :n

  def initialize(n) = @n = n
end

# a class value out of an Array, a Hash, a parameter and an instance variable
puts [Stat].first.exception(3).n
puts [Stat].first.exception(3).twice
puts [Stat, Other].first.exception(4).n
puts [Below].first.exception(5).twice
puts({ k: Stat }[:k].exception(6).n)
def build(k, v) = k.exception(v)
puts build(Stat, 7).n
puts build(Below, 8).twice
puts build(Other, 9).m
class Holder
  def initialize(k) = @k = k
  def make(v) = @k.exception(v)
end
puts Holder.new(Stat).make(10).n

# through a local, a map and a block
x = [Stat].first.exception(11)
puts x.class
puts x.message
puts x.n
puts [Stat, Below].map { |k| k.exception(12).n }.inspect
puts [Stat, Below].map { |k| k.exception(13).twice }.inspect
[Other].each { |k| puts k.exception(14).m }

# a rescued exception's class
begin
  raise Stat.new(2)
rescue => e
  k = e.class
  puts k.exception(15).n
  puts e.class.exception(16).twice
end

# by name, and asked for by a send
puts Stat.exception(17).n
puts [Stat.exception(18), 5].first.n
puts [Below.exception(19), Other.exception(20)].first.twice
puts [Stat.bare(21), 5].first.n
puts [Stat.on_self(22), 5].first.n
puts [Below.bare(23), 5].first.twice
puts [Below.on_self(24), 5].first.twice
puts [Stat.by_send(25), 5].first.n
puts [Stat::BUILT, 5].first.n
puts [Stat].first.send(:exception, 26).n
which = :exception
puts [Stat].first.send(which, 27).n

# an instance's #exception keeps its class and what it holds
puts [Stat.new(28), 5].first.exception("copied").n
puts [Stat, Stat.new(29)].map { |v| v.exception(30).n }.inspect
puts [Pair.new(31), Stat.exception(32)].map(&:n).inspect
