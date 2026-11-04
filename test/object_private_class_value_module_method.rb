# A private method of a program's Object reopening, called with a class
# value as the receiver of a boxed dispatch: a module's instance method of
# the name answers only the classes that extend the module, and a class
# method only its class and subclasses. Any other class value raises
# NoMethodError, as CRuby does.
class Object; private def top = "t"; end
module Mx; def top = "mx"; end
class Inc; include Mx; end
class Other; end
class Ext; extend Mx; end
class Own; def self.top = "own"; end
class Sub < Own; end
module Selfx; extend self; def top = "selfx"; end
[Inc.new, Other, 3, Ext, Own, Sub, Selfx, Inc].each do |x|
  begin
    puts x.top
  rescue NoMethodError => e
    puts e.message
  end
end
