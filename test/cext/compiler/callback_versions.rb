# frozen_string_literal: true
require_relative 'callback_versions'

# Box#value and CMade.made run only when the extension's C code calls them,
# a method's and an allocator's, which it does while the load runs; Mv.v is
# then the second def, not the one in a method nothing calls.
module Mv
  def self.v = 1
end
module Mv
  def self.v = 2
end
def never_called
  def Mv.v = 3
end
class Box
  def value = Mv.v
end
class CMade
  def self.made = puts(Mv.v)
end
p CextAsk.ask(Box.new)
CMade.new
