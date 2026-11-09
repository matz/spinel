# A reader called on self whose value takes `<<` (#11465): the method's
# Array of objects settles, as it did before calls on a call's value
# narrowed, and the program compiles.
class Detail
  def initialize(n) = @n = n
  attr_reader :n
end

class Record
  def initialize
    @details = []
  end

  def details
    @details
  end

  def add(n)
    details << Detail.new(n)
  end

  def add_self(n)
    self.details << Detail.new(n)
  end
end

r = Record.new
r.add(1)
r.add_self(2)
puts r.details.size
p r.details.map(&:n)
