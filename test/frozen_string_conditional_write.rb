class Store
  def initialize(value) = (@s = value)
  def rhs(value)
    puts "rhs"
    value
  end
  def write_or(value) = (@s ||= rhs(value))
  def write_and(value) = (@s &&= rhs(value))
end
s = +"x"
a = Store.new(nil).freeze
b = Store.new(s).freeze
begin
  a.write_or(s)
rescue FrozenError
  puts "frozen or"
end
p b.write_or(s).equal?(s)
p a.write_and(s)
begin
  b.write_and(s)
rescue FrozenError
  puts "frozen and"
end

class Words
  def set(value) = (@s = value)

  def pick(trace)
    @s ||= (trace << "r"; @other ||= +"side")
  end

  def seen = [defined?(@s), defined?(@other)]
end

trace = +""
words = Words.new
words.set(nil)
begin
  words.freeze.pick(trace)
rescue FrozenError
  puts "frozen"
end
p trace, words.seen
other = Words.new
p other.pick(trace), other.seen, trace
