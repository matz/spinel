# The arguments an empty initialize on a builtin only receives are evaluated as
# CRuby builds the argument list: a splat calls to_a (and raises what it
# raises), a keyword splat calls to_hash, a block pass calls to_proc, in that
# order, and the value is still the builtin's empty one.
class Array
  def initialize(*); end
end
class String
  def initialize(*); end
end
class Hash
  def initialize(*); end
end
class Spl
  def to_a
    $log << :to_a
    [1, 2]
  end
end
class One
  def to_a
    $log << :one
    ["x"]
  end
end
class Kw
  def to_hash
    $log << :to_hash
    { a: 1 }
  end
end
class Conv
  def to_proc
    $log << :to_proc
    proc { |x| x }
  end
end
class Bad
  def to_a
    $log << :bad
    raise "boom"
  end
end
$log = []
p Array.new(*Spl.new)
p Array.new(**Kw.new)
p Array.new(2, &Conv.new)
p Array.new(*Spl.new, **Kw.new, &Conv.new)
p Array.new(*nil)
p String.new(*One.new)
p Hash.new(*Spl.new)
begin
  Array.new(*Bad.new)
rescue => e
  p e.message
end
p $log
