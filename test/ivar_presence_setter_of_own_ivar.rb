# A program that reads ivar presence (instance_variable_defined?) asks of
# each ivar whether a write may leave it nil; for @limit that asks of
# `f.limit = @limit`, whose value is @limit read again. The ask that comes
# back answers the may-be-nil kind, and every class of the family takes the
# answer the outermost ask settles on: the compiler recursed without end.
class Other; attr_accessor :a; def initialize; @a = [1] if ARGV.size > 5; end; end
o = Other.new
o.a = [2]
p o.instance_variable_defined?(:@a)

class Base
  def initialize
    setup
  end

  def setup
    @limit = 60
  end
end

class Sub < Base
  def go(f)
    f.limit = @limit
  end
end

class Conn
  attr_accessor :limit
end

c = Conn.new
Sub.new.go(c)
puts c.limit
