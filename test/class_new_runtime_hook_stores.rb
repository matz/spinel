# An inherited hook that stores the class it is handed in a global, a
# collection a global or a constant holds, or a Set, and reads it back only
# after the evaluation, runs at each evaluation: each class is its own.
require "set"
REG = {}
class Kept
  @all = Set.new
  def self.all = @all
  def self.inherited(k)
    $last_kept = k
    ($kept ||= []) << k
    REG[k.object_id] = k
    @all.add(k)
    super
  end
end
def kept = Class.new(Kept) { def v = :kept }
k1 = kept
k2 = kept
p $last_kept == k2, k1 == k2
p $kept.size, REG.size, Kept.all.size, Kept.all.include?(k1)
p $kept.map { |c| c.new.v }, REG[k2.object_id] == k2

# a hook that calls a method of the program after storing the class where
# nothing else reads it runs at each evaluation too
class Noted
  def self.note = :noted
  def self.inherited(k)
    @last = k
    note
    super
  end
end
noted = -> { Class.new(Noted) { def w = :w } }
n1 = noted.call
n2 = noted.call
p n1 == n2, n2.new.w
