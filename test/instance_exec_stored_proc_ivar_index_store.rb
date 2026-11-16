# A stored proc instance_exec runs on objects of two classes, storing into
# their Hash ivar by a String key: the ivar read is the runtime self's Hash,
# not a String the store changes in place
class Rec
  def initialize = @headers = {}
  attr_reader :headers
end
class Rec2
  def initialize = @headers = {}
  attr_reader :headers
end
class Inv
  def initialize(s) = @s = s
  def run(r, *a) = r.instance_exec(*a, &@s)
end
blk = proc { |key, value| @headers[key.to_s.downcase] = value }
r = Rec.new
r2 = Rec2.new
Inv.new(blk).run(r, "Key", 1)
Inv.new(blk).run(r2, :Other, 2)
p r.headers, r2.headers
