# defined? of an attribute's **= is "assignment", as for any other operator
# write: under defined? nothing runs, so the write is not made a setter call.
class O
  attr_accessor :n
  def initialize = @n = 2
end
obj = O.new
p defined?(obj.n **= -1)
p defined?(obj.n += 1)
p defined?([obj][0].n **= 2)
obj.n **= -1
p obj.n
