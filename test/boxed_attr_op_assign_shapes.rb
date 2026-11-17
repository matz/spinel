# A boxed `recv.attr op= v` over a Struct member, an inherited accessor, a module accessor and
# an accessor declared late: CRuby calls the reader first and then the writer.

S = Struct.new(:n)
class Base
  attr_accessor :n
end
class Kid < Base
  def initialize = @n = 100
end
module HasN
  attr_accessor :n
end
class Mix
  include HasN
  def initialize = @n = 1000
end
class Late
  def initialize = @n = 5
end
box = [S.new(1), Kid.new, Mix.new, Late.new]
box.each do |o|
  begin
    o.n += 1
    o.n ||= 0
    o.n &&= o.n * 2
    p o.n
  rescue NoMethodError => e
    p [o.class, e.name]
  end
end
class Late
  attr_reader :n
end
box.each { |o| (o.n += 1; p o.n) rescue p [o.class, :late] }
if box.size > 3
  class Late
    attr_writer :n
  end
end
box.each { |o| o.n += 1; p o.n }
