# `x.to_s` on a box, where a to_s of the program's own answers a box too,
# answers a String receiver itself: stored into an Array literal it is the
# String the box holds, not a copy, also when the receiver widened to the
# box only after the route was recorded (here through the Array `names[0]`
# comes from). The shape is a Rails app's `target.is_a?(Array) ?
# target.map { |c| c.to_s } : [target.to_s]`, refused before (#8328).
class Domain
  def initialize(name) = @name = name
  def qualify = (@name << ".com"; nil)
end

class Column
  def initialize(name) = @name = name
  def to_s = @name
end

def target_names(target)
  target.is_a?(Array) ? target.map { |c| c.to_s } : [target.to_s]
end

name = +"example"
Domain.new(name).qualify
names = [name]
target = names[0]
p target_names(target)
p target_names(target)[0].equal?(target)
p Column.new(names[0]).to_s
