# A value that may be a user object with its own to_h, or an Array.
class Pairs
  def to_h = {pairs: 1}
end
[Pairs.new, [[1, 2, 3]], [1, 2], [[1, 2]]].each do |x|
  begin
    p x.to_h
  rescue => e
    p [e.class, e.message]
  end
end
