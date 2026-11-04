# A splat into a class method with a version entry, on a receiver that may
# also be another class's instance: the entry's arm gathers the splat.
class Rel
  def pluck(*cols) = cols
end

class Late
end

class Twice
  def self.pluck(col) = [col, 1]
end

def pluck_of(list, *keys)
  list.pluck(*keys)
end

p pluck_of(ARGV.empty? ? Rel.new : Late, :a)
begin
  p pluck_of(Late, :a)
rescue NoMethodError => e
  p e.message
end
p pluck_of(ARGV.empty? ? Rel.new : Twice, :b)
p pluck_of(Twice, :b)

class Late
  def self.pluck(col) = [col, :late]
end

class Twice
  def self.pluck(col) = [col, 2]
end

p pluck_of(Late, :c)
p pluck_of(Twice, :c)
p pluck_of(Rel.new, :d, :e)
