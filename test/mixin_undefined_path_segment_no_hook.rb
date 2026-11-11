# An include of a constant path whose lookup fails past its first name
# (`Known::Thing`, `Known::Mid::Thing`) raises NameError where it runs and
# mixes nothing in, so it defines no hook: another module's `included`
# that extends its includer still compiles.
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

module Other
  module Thing
  end
end

module Known
  module Here
    def here = "here"
  end
end

module Gadget
  begin
    include Known::Thing
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
  begin
    include Known::Mid::Thing
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
  include Known::Here
end

class Box
  include Gadget
end

puts Widget.label
puts Box.new.here
