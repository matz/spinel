# A boxed `recv.attr op= v` calls the reader first: a writer-only class and nil raise
# NoMethodError for `attr`.

class W1
  attr_writer :n
  def initialize = @n = 1
end
class W2
  attr_accessor :n
  def initialize = @n = 2
end
box = [W1.new, W2.new, nil]
2.times do
  box.each do |o|
    begin
      o.n += 1
      p o.n
    rescue NoMethodError => e
      p e.name
    end
  end
end
class W1
  attr_reader :n
end
box.each { |o| (o.n += 1; p o.n) rescue p :nil }
def bump(o)
  o.n += 10
end
r = begin
  bump(box[0])
rescue NoMethodError
  :x
end
p r, bump(box[1])
