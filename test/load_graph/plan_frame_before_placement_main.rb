# A `method_defined?` guard around a def asks the load record's order before
# the placement rounds run, and the call plan's frame is copied from the map
# as it then stands: a library a class body alone requires holds a region of
# its own. The rounds then place it at the class body's require and the map
# is made again with fewer regions; a question asked in the frame afterwards
# must size its run points by the frame's own region count, not the live
# map's.
class Stamp
  require_relative "plan_frame_before_placement_lib_base"
  def tag = "tag" unless Stamp.method_defined?(:tag)
end
class Stamp
  alias_method :default_inspect, :inspect
  def inspect = "stamp(#{default_inspect})"
  def tag = "later" unless Stamp.method_defined?(:tag)
end
p Stamp.new.inspect
p Stamp.new.tag
