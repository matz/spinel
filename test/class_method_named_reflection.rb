# Class methods of the program named like Class and Module reflection answer
# what they answer.
class K
  def self.ancestors = :mine
  def self.subclasses = %w[x y]
  def self.instance_methods(all = true) = all ? :all : :own
end
class Sub < K; end
p K.ancestors
p K.subclasses.map(&:upcase)
p Sub.subclasses
p K.instance_methods(false)
p K.instance_methods

module M
  def self.included_modules = 3
end
p M.included_modules + 1

class Plain; def foo; end; end
p Plain.instance_methods(false)
p Plain.ancestors.first
