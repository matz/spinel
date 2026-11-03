# spinel: reject-subclass: enters or is looked up in a Set, in a program that calls Set#compare_by_identity
# A Set subclass's own method_added hears the name of the subclass's
# compare_by_identity, and a send hands that name on: the send makes an
# identity Set, so a String member is refused at compile time.
require "set"

$added = []

class Tracked < Set
  def self.method_added(name)
    $added << name
    super
  end

  def compare_by_identity = super
end

t = Tracked.new
t.send($added.last)
t << "a"
p t.compare_by_identity?, t.include?("a")
