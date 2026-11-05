# A class method named like Class reflection whose `super` reaches a class
# method of the program above it runs, and so does that chain.
class Parent
  def self.private_instance_methods(all = false) = [:parent]
end
class Child < Parent
  def self.private_instance_methods(all = false) = super + [:child]
end
p Child.private_instance_methods(false)
p Parent.private_instance_methods(false)

class GP; def self.included_modules = [:gp]; end
class P2 < GP; def self.included_modules = super + [:p]; end
class C2 < P2; def self.included_modules = super + [:c]; end
p C2.included_modules
