class L
  def equal?(o) = true
  def now = "L#now"
end
[Time, L.new].each do |v|
  r = v.now
  puts r.is_a?(Time) ? "a Time" : r
end

class M2
  def self.equal?(o) = true
  def self.now = "M2.now"
end
class L2
  def equal?(o) = false
  def now = "L2#now"
  def utc = "L2#utc"
end
[Time, L2.new, M2].each do |v|
  r = v.now
  puts r.is_a?(Time) ? "a Time" : r
end
