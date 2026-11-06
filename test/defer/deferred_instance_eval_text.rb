# Compiled with --defer-refusals: instance_eval of a String raises
# NotImplementedError where it runs, rather than being dropped. A class-body
# macro whose module instance_evals a String on the class is not expanded
# into the body either: in CRuby it defines a singleton method of the class,
# so an instance still has no such method. One in a class body raises at its
# own statement, after the body's statements before it, and a class of the
# program that defines its own instance_eval does not exempt any other call.
# Without the flag it is refused.
# spinel: defer-refusals: 4:start true NoMethodError true body true :mine after
module Classy
  def classy(n) = instance_eval("def #{n}; 1; end")
end
puts "start"
begin
  class ClassyBox
    extend Classy
    classy :k
  end
rescue NotImplementedError => e
  puts e.message.include?("instance_eval of a String")
end
begin
  p ClassyBox.new.k
rescue NoMethodError
  puts "NoMethodError"
end
begin
  Object.new.instance_eval("@z = 1")
rescue NotImplementedError => e
  puts e.message.include?("instance_eval of a String")
end
begin
  class Direct
    puts "body"
    instance_eval("def self.k = 1")
  end
rescue NotImplementedError => e
  puts e.message.include?("instance_eval of a String")
end
class Unrelated
  def instance_eval(s) = :mine
end
p Unrelated.new.instance_eval("x")
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
