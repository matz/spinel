class E3 < StandardError; end
begin
  raise E3, "x"
rescue => e
  p [e.class, e.message]
end
class E3
  def self.exception(m = nil) = RuntimeError.new("via exception: #{m}")
end
begin
  raise E3, "y"
rescue => e
  p [e.class, e.message]
end
