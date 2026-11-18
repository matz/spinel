# A core Exception reopening can return a Ruby value of any class.
class StandardError
  def to_s = 17
end

class RuntimeError
  def to_s = :rendered
end

other = ArgumentError.new("argument")
runtime = RuntimeError.new("runtime")
p other.message, other.to_s, runtime.message, runtime.to_s
