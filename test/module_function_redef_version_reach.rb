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

# A plain def after a module function of the name is the includer's later
# version; its super reaches past the module, to the superclass's method,
# not the module function's earlier copy.
class Root
  def tag = :root
end

module Tagging
  module_function

  def tag = :module
end

module Tagging
  public

  def tag = [:own, super]
end

class Leaf < Root
  include Tagging
end

p Leaf.new.tag, Tagging.tag

# An instance def, then a module function, then a plain def calling super:
# one history of the name, its last version's super past the module, for
# an include in the class's first body and in a later one.
class Base2
  def mark = :base
end

module Marking
  def mark = :old
  module_function

  def mark = :module
end

module Marking
  public

  def mark = [:own, super]
end

class Early < Base2
  include Marking
end

class Late < Base2
end

class Late
  include Marking
end

p Early.new.mark, Late.new.mark, Marking.mark
