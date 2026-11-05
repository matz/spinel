require "json"
class Limit
  def to_int
    2
  end
end
begin
  JSON.parse("[[[0]]]", max_nesting: Limit.new)
rescue => error
  puts error.class
  puts error.message
end
