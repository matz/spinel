# An include of a constant the program defines nowhere raises NameError where
# it runs and mixes nothing in, so it gives the module no included hook:
# another module's hook stays its own (#11132).
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
    include Missing::Thing
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
end

class Box
  include Gadget
end

puts Widget.label

begin
  include Missing
rescue NameError => e
  puts "top #{e.message}"
end
class Kd
  begin
    include Mod::Missing2
  rescue NameError => e
    puts e.message
  end
end
