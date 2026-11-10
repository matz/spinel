# A class method of the program's named like Class's own (Module#include?)
# answers a Class value known only at run time, where its class has one; a
# class without one answers by Module's.
module Viewing; end
module Redirects; end

class Base
  include Viewing
  include Redirects
end

class Api < Base
  def self.include?(mod)
    mod != Viewing && ancestors.include?(mod)
  end
end

class Pings < Api; end
class Pages < Base; end

def views?(controller)
  controller.class.include?(Viewing)
end

def redirects?(controller)
  controller.class.include?(Redirects)
end

p views?(Pings.new)
p views?(Pages.new)
p redirects?(Pings.new)
p redirects?(Pages.new)
[Pings.new, Pages.new, Base.new].each { |c| p [c.class.name, c.class.include?(Viewing), c.class.include?(Comparable)] }
p Pings.include?(Viewing)
p Pages.include?(Viewing)
