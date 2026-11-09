# A method a program adds to Comparable reads its receiver's class: a
# Rational's is Numeric's subclass, and every receiver's class includes
# Comparable.
module Comparable
  def own_class_facts = [self.class.superclass, self.class.include?(Comparable), self.class.ancestors.size, self.class < Numeric]
end
p Rational(3, 4).own_class_facts, Time.at(0).own_class_facts, "s".own_class_facts, :s.own_class_facts, 3.own_class_facts
