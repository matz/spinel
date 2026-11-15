# An include or extend naming a constant whose first name the program
# defines only further down raises NameError where it runs, as CRuby does,
# and mixes nothing in: another module's `included` hook is not reached
# through it, and a name defined before its use is unchanged.
module Hooky
  def self.included(base)
    base.extend(LabelMethods)
  end

  module LabelMethods
    def label = "widget"
  end
end

class Widget
  include Hooky
end

module Gadget
  begin
    include Known::Thing
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
end

module Known
end

class Box
  include Gadget
end

puts Widget.label

# a bare first name defined further down, at the top level and in a class body
begin
  include TopLater
rescue NameError => e
  puts "NameError: #{e.message}"
end
module TopLater; end

class Uses
  begin
    include Deep::Inner
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
  begin
    extend AlsoLater
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
end
module Deep
  module Inner; end
end
module AlsoLater
  def tag = :t
end

# defined before: unchanged
module Early
  def e = :early
end
class Uses
  include Early
end
p Uses.new.e
