class E1 < StandardError
  def initialize(m) = super("e1:#{m}")
end
k = [E1, ArgumentError][ARGV.size]
begin
  raise k, "a"
rescue => e
  p e.message
end
class E1
  def initialize(m) = super("E1v2:#{m.upcase}")
end
begin
  raise k, "b"
rescue => e
  p e.message
end
