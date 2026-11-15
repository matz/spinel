# An include at the top level adds the module to Object. An instance of a
# BasicObject subclass is not an Object, so a call in its instance method
# does not reach the module: NoMethodError where it runs, written with
# parentheses or without (the parenless spelling:
# test/basicobject_toplevel_include_class_method.rb).
module T
  module_function

  def hello = "hi"
end

include T

class Blank < BasicObject
  def greet
    hello()
  rescue ::NoMethodError => e
    e.message
  end
end

$stdout.puts Blank.new.greet
$stdout.puts hello
$stdout.puts 1.send(:hello)
