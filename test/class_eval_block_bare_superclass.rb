# In a class_eval block of the body's own class a bare superclass is the
# class's, and a def's super reaches the superclass's method.
class B; def m = "b"; end
class A < B
  class_eval do
    def m = "a" + super
    puts "sup #{superclass}"
  end
end
p A.new.m
