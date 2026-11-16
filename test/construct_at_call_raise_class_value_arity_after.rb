class E5 < StandardError; end
k = [E5, IOError][ARGV.size]
begin
  raise k
rescue => e
  p e.message
end
class E5
  def initialize(a, b = 2) = super("#{a}-#{b}")
end
begin
  raise k
rescue ArgumentError => e
  p e.class
end
begin
  raise k, "m"
rescue => e
  p e.message
end
