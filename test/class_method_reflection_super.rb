# `super` in a class method named superclass or ancestors reaches
# Class#superclass or Module#ancestors of the receiving class.
class Base
  def self.superclass = super
  def self.ancestors = super.take(2)
end
class Sub < Base; end
p Sub.superclass
p Base.superclass
p Sub.ancestors
p Base.ancestors

module M
  def self.ancestors = super
end
p M.ancestors

module N
  def self.superclass = super
end
p((N.superclass rescue $!.class))
