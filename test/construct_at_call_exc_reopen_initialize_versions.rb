begin; raise ArgumentError, "a"; rescue => e; p e.message; end
class ArgumentError
  def initialize(m = "z") = super("v1:#{m}")
end
begin; raise ArgumentError, "b"; rescue => e; p e.message; end
p ArgumentError.new("n").message
class ArgumentError
  def initialize(m = "z") = super("v2:#{m}")
end
begin; raise ArgumentError, "c"; rescue => e; p e.message; end
begin; raise ArgumentError; rescue => e; p e.message; end
p ArgumentError.new("o").message
