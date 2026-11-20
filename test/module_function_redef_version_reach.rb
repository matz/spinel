# A module's instance method that the module later defines again under
# module_function stays the includer's public method until that def runs:
# a call before it answers the first version, a call after it finds the
# module function's private instance side, and a call from inside the
# includer answers whichever version is live then. A later plain def of
# the name is public again, while the module's own function keeps its body.

module Util
  def line = :first
end

class Base
  include Util

  def show = line
end

p Base.new.line, Base.new.show

module Util
  module_function

  def line = :second
end

begin
  Base.new.line
rescue NoMethodError => e
  p e.class
end
p Base.new.show, Util.line

module Util
  def line = :third
end

p Base.new.line, Base.new.show, Util.line

module Tools
  def pick = :first
end

class Kit
  include Tools
end

calls = [Kit.new.pick]

module Tools
  def pick = :second
  module_function :pick
end

calls << (Kit.new.pick rescue :private)
calls << Tools.pick
p calls
