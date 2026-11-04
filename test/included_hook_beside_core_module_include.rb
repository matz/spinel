# A module's `included` hook that extends its includer is spliced into the
# includer's body, also when another module of the program includes a core
# module (Comparable, Enumerable): that include is code the load record
# does not hold, but it gives the module no hook of its own, so the record
# must not take Gizmo's or Gadget's include for one that may run a program
# hook. It did, and the hook ran as a call, refused as Object#extend
# (#11040).
module Hooky
  def self.included(base)
    base.extend(LabelMethods)
  end
  module LabelMethods
    def label = "widget"
  end
end
class Widget
  include Hooky
end
module Gizmo
  include Comparable
  def <=>(o) = 0
end
class Gadget
  include Gizmo
end
module Bag
  include Enumerable
  def each = yield(1)
end
class Sack
  include Bag
end
puts Widget.label
p Gadget.new == Gadget.new
p Sack.new.to_a
