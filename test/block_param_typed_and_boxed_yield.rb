# A block passed through virtual dispatch must bind its parameters from every
# yielding implementation, including boxed cache reads, not just the base's
# typed yield. Boxed objects dispatch on their actual class and preserve nil.
class S
  def initialize(n) = @n = n
  def n = @n
end

class Base
  def query = run { |set| set.n }
  def run = yield(S.new(1))
end

class Prepared < Base
  def initialize = @h = { "k" => S.new(2) }
  def run = yield(@h["k"])
end

p Base.new.query
p Prepared.new.query

class OtherStatement
  def n = 3
end

class OtherPrepared < Base
  def initialize = @h = { "k" => OtherStatement.new }
  def run = yield(@h["k"])
end

p OtherPrepared.new.query

class NilPrepared < Base
  def initialize = @h = { "k" => nil, "other" => S.new(0) }
  def run = yield(@h["k"])
end

begin
  NilPrepared.new.query
rescue NoMethodError => e
  p e.class
end

class PairBase
  def query = run { |left, right| [left.n, right.n] }
  def run = yield(S.new(4), S.new(5))
end

class PairPrepared < PairBase
  def initialize = @h = { "left" => S.new(6), "right" => OtherStatement.new }
  def run = yield(@h["left"], @h["right"])
end

p PairBase.new.query
p PairPrepared.new.query

class StringBase
  def query = run { |text| text.upcase }
  def run = yield("typed")
end

class StringPrepared < StringBase
  def initialize = @h = { "k" => "boxed", "other" => S.new(0) }
  def run = yield(@h["k"])
end

p StringBase.new.query
p StringPrepared.new.query

class IntegerBase
  def query = run { |number| number + 1 }
  def run = yield(7)
end

class IntegerPrepared < IntegerBase
  def initialize = @h = { "k" => 8, "other" => S.new(0) }
  def run = yield(@h["k"])
end

p IntegerBase.new.query
p IntegerPrepared.new.query
