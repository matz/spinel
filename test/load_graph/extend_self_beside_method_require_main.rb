# A require inside a method body is a load the program may run later, so
# the library it names holds a region of its own. Settling the record
# across regions looks again at each mixin's module, and must keep the one
# `extend self` names: the module whose body it is written in, whatever a
# later region defines.
class Report
  def render
    require_relative "extend_self_beside_method_require_lib_pretty"
    Pretty.wrap("report")
  end
end

module Inflector
  extend self

  def pluralize(word) = "#{word}s"
  def inflections(locale = :en) = yield(locale)
end

module Numbers
  extend self
  def double(n) = n * 2
end

p Inflector.pluralize("post")
Inflector.inflections(:en) { |l| p l }
p Numbers.double(21)
p Report.new.render
