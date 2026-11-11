# A Set subclass whose own method_added hears its names, and a send of one
# it heard: Set's initialize then takes its block as a proc, and the
# class-dispatched `new` of Set.[] (a subclass's or Set's constructor by
# the receiving class) passed no block slot to either, so the C did not
# build (too few arguments to sp_Tracked_new / sp_Set_new).
require "set"
$added = []
class Tracked < Set
  def self.method_added(name)
    $added << name
    super
  end
  def foo = 1
  def compare_by_identity = super
end
t = Tracked.new
p t.send($added.first)
t << "a"
p t.include?("a"), Tracked[1, 2].size, Set[3].include?(3)
u = Tracked.new
u.send($added.last)
u << "b"
p u.compare_by_identity?, u.include?("b"), $added
