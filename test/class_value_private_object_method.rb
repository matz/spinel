# A private method of a reopened Object, called on a class value: Class's
# and Module's own public methods (name) answer first, and a builtin class
# value (Integer, String) is refused like a program class value, as in
# CRuby.
class Object
  private def name = "obj"
  private def top = 1
end
module Mx
  def name = "mx"
  def top = 2
end
class Inc; include Mx; end
class K; end
p K.name
[Inc.new, K, Integer, 3].each { |x| p((x.name rescue $!.message)) }
[Inc.new, Integer, String, K].each { |x| p((x.top rescue $!.message)) }
