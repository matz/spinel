# An exception class's #to_s or #message override is kept by reachability,
# with everything it calls, once an instance of the class can exist; a class
# nothing builds keeps neither the override nor its callees (#11625). Before,
# the text dispatcher kept every override's body alive on its own while the
# class methods it calls (`self.class.label`) were dropped, and the C
# compiler stopped on an undeclared function.

# never built: none of these is compiled, and the program still builds
class Unbuilt < StandardError
  def to_s
    self.class.label
  end
  def self.label
    "unbuilt"
  end
end

class UnbuiltInterp < StandardError
  LABEL = "const"
  def message
    "#{self.class.heading}: #{self.class::LABEL} #{helper}"
  end
  def self.heading
    "unbuilt-interp"
  end
  private
  def helper
    "helper"
  end
end

module Labels
  def self.for(k)
    "label:" + k.name
  end
end

class UnbuiltModule < StandardError
  def to_s
    Labels.for(self.class)
  end
end

# the override is on the base, the class method on the subclass
class Base < StandardError
  def to_s
    self.class.default_message + "!"
  end
  def self.default_message
    "base"
  end
end

class Sub < Base
  def self.default_message
    "sub"
  end
end

# never raised: a method that would raise it is never called
def never_called
  raise Unbuilt
end

puts "ok"

# built: the override and its callees run
begin
  raise Sub
rescue => e
  puts e.message
  puts "#{e}"
  puts e.to_s
end

begin
  raise Base, "ignored"
rescue Base => e
  puts e.message
end

class Built < StandardError
  def to_s
    prefix + ": " + self.class.label
  end
  def self.label
    "built"
  end
  private
  def prefix
    "Built"
  end
end

x = Built.exception("m")
puts x.message
puts Built.new.full_message(highlight: false).include?("Built: built")
