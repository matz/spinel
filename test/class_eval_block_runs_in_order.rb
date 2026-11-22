# A class_eval, module_eval or class_exec block on the body's own class runs
# its statements where the call stands, in order with the body's, with the
# class for self: a bare instance_methods or name there is the class's.
class A
  puts "1"
  class_eval do
    puts "2"
    def m = 1
    puts "3 #{instance_methods(false).inspect}"
  end
  puts "4"
  module_eval do
    puts "5 #{name}"
  end
  class_exec { puts "6 #{self}" }
end
