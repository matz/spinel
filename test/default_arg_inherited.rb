# Issue #49: a child class instance calling a parent method that has
# a default argument should fill in the default at the call site.

class Parent
  def hi(name = "world")
    puts "Hello, #{name}"
  end
end

class Child < Parent
end

Child.new.hi              # Hello, world
Child.new.hi("Ruby")      # Hello, Ruby
