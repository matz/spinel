# `files -= @exclude if @exclude`: a set operator assignment on a typed
# Array local whose operand is a nullable ivar (Rails' Paths#files_in)
class Paths
  def initialize(exclude, skip)
    @exclude = exclude
    @skip = skip
  end

  def files_in(names)
    files = names.map { |n| n + ".rb" }
    files -= @exclude if @exclude
    files.sort
  end

  def keep(ids)
    kept = ids.map { |i| i * 2 }
    kept -= @skip if @skip
    kept
  end
end

p Paths.new(["b.rb"], [4]).files_in(%w[a b c])
p Paths.new(nil, nil).files_in(%w[a b c])
p Paths.new(["b.rb"], [4]).keep([1, 2, 3])
p Paths.new(nil, nil).keep([1, 2, 3])
